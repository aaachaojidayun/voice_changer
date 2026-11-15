// 实时变声测试程序（Windows / WASAPI 共享模式 + 事件驱动）。
//
// 数据链路：
//   默认麦克风（采集） -> 混为单声道 -> vc::PitchShifter 变调
//   -> 复制到各声道 -> 默认播放设备（耳机）
//
// 线程模型：
//   - 处理线程：等采集事件 -> GetBuffer -> 变调 -> 写入无锁环形缓冲
//   - 播放线程：等播放事件 -> 从环形缓冲取样本 -> 写设备缓冲（不足补零）
//   COM 全部在 MTA 中创建，两个线程共享同一套 IAudioClient 接口指针
//   （MTA 对象线程间可直接使用，无需 marshalling）。
//
// 运行期切换音调：控制台输入数字（倍数，如 1.5），主线程离线构造新的
// PitchShifter，处理线程在下一个音频块开始时用 50 ms 交叉淡化切换到新实例，
// 避免直接改步长造成的爆音；旧实例延迟到退出时统一释放。

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "vc/pitch_shifter.hpp"
#include "vc/spsc_ring.hpp"

namespace {

// ---------------------------------------------------------------- 常量与 GUID
// 显式给出 GUID，避免依赖编译器的 __uuidof 扩展。
const CLSID kClsidMMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidAudioCaptureClient = {
    0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
const IID kIidAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

const PROPERTYKEY kPkeyFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

// WAVEFORMATEXTENSIBLE 的子格式（与 ksmedia.h 中的定义一致，此处自带避免额外依赖）。
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// 参考时间单位：1 秒 = 10,000,000。
const REFERENCE_TIME kHundredMs = 1000000;

// 切换音调时的交叉淡化时长。
const double kFadeSeconds = 0.05;

// ---------------------------------------------------------------- 小工具
template <class T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr,
                        nullptr);
    return s;
}

void printHr(const char* what, HRESULT hr) {
    std::printf("[错误] %s 失败，HRESULT=0x%08lX\n", what, static_cast<unsigned long>(hr));
}

bool failed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        printHr(what, hr);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 采样格式
struct FmtInfo {
    int sampleRate = 0;
    int channels = 0;
    int bytesPerSample = 0;
    bool isFloat = false;
    int bitsPerSample = 0;

    int frameBytes() const { return channels * bytesPerSample; }
};

// 解析 WAVEFORMATEX，只接受 32-bit float 与 16/32-bit 整型 PCM。
bool parseFormat(const WAVEFORMATEX* wf, FmtInfo& out) {
    if (!wf) return false;
    out.sampleRate = static_cast<int>(wf->nSamplesPerSec);
    out.channels = static_cast<int>(wf->nChannels);
    out.bitsPerSample = static_cast<int>(wf->wBitsPerSample);
    out.isFloat = false;

    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        out.isFloat = true;
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(ex->SubFormat, kSubtypeFloat)) {
            out.isFloat = true;
        } else if (!IsEqualGUID(ex->SubFormat, kSubtypePcm)) {
            return false;
        }
    } else if (wf->wFormatTag != WAVE_FORMAT_PCM) {
        return false;
    }

    if (out.channels <= 0 || out.sampleRate <= 0) return false;

    if (out.isFloat) {
        if (out.bitsPerSample != 32) return false;
        out.bytesPerSample = 4;
    } else {
        if (out.bitsPerSample == 16) {
            out.bytesPerSample = 2;
        } else if (out.bitsPerSample == 32) {
            out.bytesPerSample = 4;
        } else {
            return false;
        }
    }
    return true;
}

// 读一个样本，归一化到 float。
inline float readSample(const BYTE* p, const FmtInfo& fi) {
    if (fi.isFloat) {
        float v;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    if (fi.bytesPerSample == 2) {
        std::int16_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 32768.0f;
    }
    std::int32_t v;
    std::memcpy(&v, p, sizeof(v));
    return static_cast<float>(static_cast<double>(v) / 2147483648.0);
}

// 写一个样本，带饱和限幅。
inline void writeSample(BYTE* p, const FmtInfo& fi, float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (fi.isFloat) {
        std::memcpy(p, &v, sizeof(float));
        return;
    }
    if (fi.bytesPerSample == 2) {
        const float s = v * 32767.0f;
        const std::int16_t iv =
            static_cast<std::int16_t>(s >= 0.0f ? (s + 0.5f) : (s - 0.5f));
        std::memcpy(p, &iv, sizeof(iv));
        return;
    }
    const double s = static_cast<double>(v) * 2147483647.0;
    std::int32_t iv = static_cast<std::int32_t>(s >= 0.0 ? (s + 0.5) : (s - 0.5));
    if (iv > 2147483647) iv = 2147483647;
    if (iv < -2147483647 - 1) iv = -2147483647 - 1;
    std::memcpy(p, &iv, sizeof(iv));
}

// 交错多声道 -> 单声道（各声道取平均）。
void toMono(const BYTE* src, UINT32 frames, const FmtInfo& fi, float* dst) {
    const int ch = fi.channels;
    const int bps = fi.bytesPerSample;
    const float norm = 1.0f / static_cast<float>(ch);
    for (UINT32 i = 0; i < frames; ++i) {
        const BYTE* base = src + static_cast<std::size_t>(i) * ch * bps;
        float acc = 0.0f;
        for (int c = 0; c < ch; ++c) acc += readSample(base + static_cast<std::size_t>(c) * bps, fi);
        dst[i] = acc * norm;
    }
}

// 单声道 -> 交错多声道（各声道写同一值）。
void fromMono(const float* src, UINT32 frames, const FmtInfo& fi, BYTE* dst) {
    const int ch = fi.channels;
    const int bps = fi.bytesPerSample;
    for (UINT32 i = 0; i < frames; ++i) {
        BYTE* base = dst + static_cast<std::size_t>(i) * ch * bps;
        for (int c = 0; c < ch; ++c) writeSample(base + static_cast<std::size_t>(c) * bps, fi, src[i]);
    }
}

// ---------------------------------------------------------------- 设备与会话
struct Endpoint {
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0;
    WAVEFORMATEX* format = nullptr;  // CoTaskMemAlloc 分配，需 CoTaskMemFree
    FmtInfo info;
    std::string name;
};

struct Session {
    IMMDeviceEnumerator* enumerator = nullptr;
    Endpoint capture;
    Endpoint render;
};

std::string deviceFriendlyName(IMMDevice* dev) {
    IPropertyStore* store = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store)) || !store) return "(未知设备)";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name = "(未知设备)";
    if (SUCCEEDED(store->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
        name = toUtf8(v.pwszVal);
    }
    PropVariantClear(&v);
    store->Release();
    return name;
}

struct DeviceItem {
    std::wstring id;
    std::string name;
    bool isDefault = false;
};

// 枚举当前处于活动状态的全部端点（顺序与 --list 输出一致）。
std::vector<DeviceItem> enumerateDevices(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
    std::vector<DeviceItem> items;
    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) ||
        !collection) {
        return items;
    }

    std::wstring defaultId;
    IMMDevice* defaultDev = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDev)) && defaultDev) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDev->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
        defaultDev->Release();
    }

    UINT32 count = 0;
    if (FAILED(collection->GetCount(&count))) count = 0;
    for (UINT32 i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(collection->Item(i, &dev)) || !dev) continue;
        DeviceItem item;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            item.id = id;
            CoTaskMemFree(id);
        }
        item.name = deviceFriendlyName(dev);
        item.isDefault = (!defaultId.empty() && item.id == defaultId);
        dev->Release();
        items.push_back(std::move(item));
    }
    collection->Release();
    return items;
}

bool openEndpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow, const char* tag, Endpoint& ep,
                  const std::wstring* deviceId) {
    HRESULT hr;
    if (deviceId && !deviceId->empty()) {
        hr = enumerator->GetDevice(deviceId->c_str(), &ep.device);
        if (failed(hr, "GetDevice")) {
            std::printf("        -> 无法打开指定的%s设备，可能已被拔出。\n", tag);
            return false;
        }
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &ep.device);
        if (failed(hr, tag)) {
            std::printf(
                "        -> 没有找到默认%s设备，请在系统声音设置里确认设备已连接并被设为默认。\n",
                tag);
            return false;
        }
    }
    ep.name = deviceFriendlyName(ep.device);

    hr = ep.device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&ep.client));
    if (failed(hr, "Activate(IAudioClient)")) return false;

    hr = ep.client->GetMixFormat(&ep.format);
    if (failed(hr, "GetMixFormat")) return false;

    if (!parseFormat(ep.format, ep.info)) {
        std::printf("[错误] %s设备混音格式不受支持：%u Hz / %u 声道 / %u bit（仅支持 "
                    "float32 或 16/32-bit PCM）\n",
                    tag, static_cast<unsigned>(ep.format->nSamplesPerSec),
                    static_cast<unsigned>(ep.format->nChannels),
                    static_cast<unsigned>(ep.format->wBitsPerSample));
        return false;
    }

    // 共享模式 + 事件回调。共享模式下请求缓冲时长由音频引擎决定。
    hr = ep.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               kHundredMs, 0, ep.format, nullptr);
    if (failed(hr, "Initialize")) return false;

    ep.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ep.event) {
        std::printf("[错误] CreateEvent 失败\n");
        return false;
    }
    hr = ep.client->SetEventHandle(ep.event);
    if (failed(hr, "SetEventHandle")) return false;

    hr = ep.client->GetBufferSize(&ep.bufferFrames);
    if (failed(hr, "GetBufferSize")) return false;

    return true;
}

void closeEndpoint(Endpoint& ep) {
    if (ep.client) ep.client->Stop();
    safeRelease(ep.client);
    safeRelease(ep.device);
    if (ep.event) {
        CloseHandle(ep.event);
        ep.event = nullptr;
    }
    if (ep.format) {
        CoTaskMemFree(ep.format);
        ep.format = nullptr;
    }
}

// ---------------------------------------------------------------- 运行期状态
struct LiveState {
    std::atomic<bool> stop{false};
    std::atomic<vc::PitchShifter*> active{nullptr};   // 处理线程当前使用的实例
    std::atomic<vc::PitchShifter*> pending{nullptr};  // 待切换实例（由主线程投递）
    std::atomic<unsigned long long> underruns{0};
    std::atomic<unsigned long long> overruns{0};
    std::atomic<unsigned long long> blocks{0};
    std::atomic<bool> primed{false};  // 采集端已产出第一块数据
    std::atomic<float> inRms{0.0f};   // 输入单声道 RMS（判断麦克风是否采到声音）
    std::atomic<float> outPeak{0.0f}; // 输出峰值（判断是否有信号送给耳机）
    vc::SpscRing ring;
    int sampleRate = 48000;
};

volatile LONG g_ctrlStop = 0;

BOOL WINAPI consoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            InterlockedExchange(&g_ctrlStop, 1);
            return TRUE;
        default:
            return FALSE;
    }
}

// ---------------------------------------------------------------- 采集线程
void captureLoop(Session* s, LiveState* st, int fftSize, float initialPitch) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IAudioCaptureClient* capture = nullptr;
    HRESULT hr = s->capture.client->GetService(
        kIidAudioCaptureClient, reinterpret_cast<void**>(&capture));
    if (failed(hr, "GetService(IAudioCaptureClient)")) return;

    const FmtInfo& cf = s->capture.info;

    // 预分配全部工作缓冲，循环内不再分配。
    std::vector<float> mono(s->capture.bufferFrames > 0 ? s->capture.bufferFrames : 1024);
    std::vector<float> outA(mono.size());
    std::vector<float> outB(mono.size());

    auto* current = new vc::PitchShifter(static_cast<float>(st->sampleRate), initialPitch, fftSize);
    st->active.store(current, std::memory_order_release);

    const int fadeTotal = std::max(1, static_cast<int>(st->sampleRate * kFadeSeconds));
    int fadeRemain = 0;
    vc::PitchShifter* incoming = nullptr;

    hr = s->capture.client->Start();
    if (failed(hr, "IAudioClient::Start(capture)")) {
        capture->Release();
        return;
    }

    bool firstBlock = true;

    while (!st->stop.load(std::memory_order_relaxed)) {
        const DWORD wait = WaitForSingleObject(s->capture.event, 200);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;

        UINT32 packet = 0;
        hr = capture->GetNextPacketSize(&packet);
        if (FAILED(hr)) break;

        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (frames > 0) {
                if (mono.size() < frames) {  // 设备周期变化时的兜底（极少发生）
                    mono.resize(frames);
                    outA.resize(frames);
                    outB.resize(frames);
                }

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(mono.begin(), mono.begin() + frames, 0.0f);
                } else {
                    toMono(data, frames, cf, mono.data());
                }

                // 处理线程收到切换请求：本块开始交叉淡化到新实例。
                if (!incoming) {
                    vc::PitchShifter* want = st->pending.exchange(nullptr, std::memory_order_acq_rel);
                    if (want && want != current) {
                        incoming = want;
                        fadeRemain = fadeTotal;
                    } else if (want) {
                        // 与当前实例相同（重复设置），丢弃。
                        delete want;
                    }
                }

                current->process(mono.data(), outA.data(), frames);

                if (incoming) {
                    incoming->process(mono.data(), outB.data(), frames);
                    const float step = 1.0f / static_cast<float>(fadeTotal);
                    float g = static_cast<float>(fadeTotal - fadeRemain) * step;
                    for (UINT32 i = 0; i < frames; ++i) {
                        const float gi = (g > 1.0f) ? 1.0f : g;
                        outA[i] = outA[i] * (1.0f - gi) + outB[i] * gi;
                        g += step;
                    }
                    fadeRemain -= static_cast<int>(frames);
                    if (fadeRemain <= 0) {
                        // 切换完成：旧实例退役，新实例转正。
                        st->active.store(incoming, std::memory_order_release);
                        current = incoming;
                        incoming = nullptr;
                        fadeRemain = 0;
                    }
                }

                float sumSq = 0.0f;
                float peak = 0.0f;
                for (UINT32 i = 0; i < frames; ++i) {
                    sumSq += mono[i] * mono[i];
                    peak = std::max(peak, std::fabs(outA[i]));
                }
                st->inRms.store(std::sqrt(sumSq / static_cast<float>(frames)),
                                std::memory_order_relaxed);
                st->outPeak.store(peak, std::memory_order_relaxed);

                const std::size_t wrote = st->ring.write(outA.data(), frames);
                if (wrote < frames) st->overruns.fetch_add(1, std::memory_order_relaxed);
                st->blocks.fetch_add(1, std::memory_order_relaxed);
                if (firstBlock) {
                    firstBlock = false;
                    st->primed.store(true, std::memory_order_release);
                }
            }

            capture->ReleaseBuffer(frames);
            hr = capture->GetNextPacketSize(&packet);
            if (FAILED(hr)) break;
        }
    }

    s->capture.client->Stop();
    capture->Release();
    // current 由主线程在汇总处统一释放，这里不 delete。
}

// ---------------------------------------------------------------- 播放线程
void renderLoop(Session* s, LiveState* st) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IAudioRenderClient* render = nullptr;
    HRESULT hr = s->render.client->GetService(
        kIidAudioRenderClient, reinterpret_cast<void**>(&render));
    if (failed(hr, "GetService(IAudioRenderClient)")) return;

    const FmtInfo& rf = s->render.info;
    const UINT32 bufFrames = s->render.bufferFrames;

    std::vector<float> mono(bufFrames > 0 ? bufFrames : 1024);
    std::vector<BYTE> bytes(static_cast<std::size_t>(mono.size()) *
                            static_cast<std::size_t>(rf.frameBytes()));

    // 先整体填一段静音，避免 Start 后立刻欠载。
    {
        BYTE* p = nullptr;
        hr = render->GetBuffer(bufFrames, &p);
        if (SUCCEEDED(hr)) {
            hr = render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            if (failed(hr, "ReleaseBuffer(silence)")) {
                render->Release();
                return;
            }
        }
    }

    hr = s->render.client->Start();
    if (failed(hr, "IAudioClient::Start(render)")) {
        render->Release();
        return;
    }

    while (!st->stop.load(std::memory_order_relaxed)) {
        const DWORD wait = WaitForSingleObject(s->render.event, 200);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;

        UINT32 padding = 0;
        hr = s->render.client->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;

        UINT32 avail = bufFrames - padding;
        if (avail == 0) continue;

        if (mono.size() < avail) {
            mono.resize(avail);
            bytes.resize(static_cast<std::size_t>(avail) *
                         static_cast<std::size_t>(rf.frameBytes()));
        }

        const std::size_t got = st->ring.read(mono.data(), avail);
        if (got < avail) {
            std::fill(mono.begin() + static_cast<std::ptrdiff_t>(got),
                      mono.begin() + static_cast<std::ptrdiff_t>(avail), 0.0f);
            st->underruns.fetch_add(1, std::memory_order_relaxed);
        }

        BYTE* p = nullptr;
        hr = render->GetBuffer(avail, &p);
        if (FAILED(hr)) break;

        fromMono(mono.data(), avail, rf, p);

        hr = render->ReleaseBuffer(avail, 0);
        if (FAILED(hr)) break;
    }

    s->render.client->Stop();
    render->Release();
}

// ---------------------------------------------------------------- 参数解析
void printUsage() {
    std::printf(
        "用法: vc_live [选项]\n"
        "\n"
        "  --list            列出可用的麦克风与播放设备（含编号）后退出\n"
        "  --capture N       用编号为 N 的麦克风（默认用系统默认麦克风）\n"
        "  --render N        用编号为 N 的播放设备（默认用系统默认播放设备）\n"
        "  --pitch 倍数      初始变调倍数，默认 1.5（>1 变尖、<1 变低沉）\n"
        "  --fft N           Phase Vocoder 的 FFT 长度，默认 1024（越大越稳、延迟越高）\n"
        "\n"
        "运行时在控制台输入数字并回车可实时切换音调倍数，输入 s 查看运行状态，输入 q 退出。\n");
}

// 主线程的调试信息打印。
void printOneLineStatus(const LiveState& st) {
    const float inRms = st.inRms.load();
    const float outPeak = st.outPeak.load();
    const float inDb = 20.0f * std::log10(inRms + 1e-12f);
    const float outDb = 20.0f * std::log10(outPeak + 1e-12f);
    std::printf("    块数=%llu 欠载=%llu 溢出=%llu 积压=%zu 样本 | 麦克风输入 %.1f dBFS 耳机输出峰 %.1f dBFS\n",
                static_cast<unsigned long long>(st.blocks.load()),
                static_cast<unsigned long long>(st.underruns.load()),
                static_cast<unsigned long long>(st.overruns.load()), st.ring.available(),
                static_cast<double>(inDb), static_cast<double>(outDb));
    if (inDb < -60.0f) {
        std::printf("    [!] 麦克风输入几乎为静音：检查麦克风是否被静音/增益为 0，或换 --capture 编号\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    float pitch = 1.5f;
    int fftSize = 1024;
    bool listOnly = false;
    int captureIndex = -1;
    int renderIndex = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--pitch" && i + 1 < argc) {
            pitch = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--fft" && i + 1 < argc) {
            fftSize = std::atoi(argv[++i]);
        } else if (arg == "--capture" && i + 1 < argc) {
            captureIndex = std::atoi(argv[++i]);
        } else if (arg == "--render" && i + 1 < argc) {
            renderIndex = std::atoi(argv[++i]);
        } else if (arg == "--list") {
            listOnly = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else {
            std::printf("[错误] 未知参数: %s\n\n", arg.c_str());
            printUsage();
            return 1;
        }
    }
    if (!(pitch > 0.05f && pitch < 8.0f)) {
        std::printf("[错误] --pitch 需在 0.05 ~ 8.0 之间\n");
        return 1;
    }
    if (fftSize != 512 && fftSize != 1024 && fftSize != 2048 && fftSize != 4096) {
        std::printf("[错误] --fft 只支持 512 / 1024 / 2048 / 4096\n");
        return 1;
    }

    std::printf("=== 实时变声测试（WASAPI 共享模式） ===\n\n");

    // COM：MTA。返回 S_FALSE 表示已初始化，同样可用。
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        std::printf("[错误] 当前线程已用 STA 初始化 COM，无法切换到 MTA\n");
        return 1;
    }

    Session session;
    hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidMMDeviceEnumerator,
                          reinterpret_cast<void**>(&session.enumerator));
    if (failed(hr, "CoCreateInstance(MMDeviceEnumerator)")) return 1;

    // 枚举可用端点，供 --list 与 --capture/--render 使用。
    const std::vector<DeviceItem> caps = enumerateDevices(session.enumerator, eCapture);
    const std::vector<DeviceItem> rens = enumerateDevices(session.enumerator, eRender);

    auto printList = [](const char* title, const std::vector<DeviceItem>& items) {
        std::printf("\n%s:\n", title);
        if (items.empty()) {
            std::printf("  (无可用设备)\n");
            return;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            std::printf("  [%zu] %s%s\n", i, items[i].name.c_str(),
                        items[i].isDefault ? "  (默认)" : "");
        }
    };

    if (listOnly) {
        printList("麦克风", caps);
        printList("播放设备", rens);
        std::printf("\n用 --capture N / --render N 指定设备编号。\n");
        safeRelease(session.enumerator);
        CoUninitialize();
        return 0;
    }

    const std::wstring* capId = nullptr;
    const std::wstring* renId = nullptr;

    if (captureIndex >= 0) {
        if (static_cast<std::size_t>(captureIndex) >= caps.size()) {
            std::printf("[错误] --capture %d 超出范围（共 %zu 个麦克风），用 --list 查看编号。\n",
                        captureIndex, caps.size());
            safeRelease(session.enumerator);
            CoUninitialize();
            return 1;
        }
        capId = &caps[static_cast<std::size_t>(captureIndex)].id;
    }
    if (renderIndex >= 0) {
        if (static_cast<std::size_t>(renderIndex) >= rens.size()) {
            std::printf("[错误] --render %d 超出范围（共 %zu 个播放设备），用 --list 查看编号。\n",
                        renderIndex, rens.size());
            safeRelease(session.enumerator);
            CoUninitialize();
            return 1;
        }
        renId = &rens[static_cast<std::size_t>(renderIndex)].id;
    }

    std::printf("[1/4] 打开麦克风与播放设备...\n");
    if (!openEndpoint(session.enumerator, eCapture, "采集", session.capture, capId)) {
        closeEndpoint(session.capture);
        safeRelease(session.enumerator);
        CoUninitialize();
        return 1;
    }
    std::printf("      麦克风: %s\n", session.capture.name.c_str());
    std::printf("        格式: %d Hz / %d 声道 / %s\n", session.capture.info.sampleRate,
                session.capture.info.channels, session.capture.info.isFloat ? "float32" : "PCM");

    if (!openEndpoint(session.enumerator, eRender, "播放", session.render, renId)) {
        closeEndpoint(session.capture);
        closeEndpoint(session.render);
        safeRelease(session.enumerator);
        CoUninitialize();
        return 1;
    }
    std::printf("      播放设备: %s\n", session.render.name.c_str());
    std::printf("        格式: %d Hz / %d 声道 / %s\n", session.render.info.sampleRate,
                session.render.info.channels, session.render.info.isFloat ? "float32" : "PCM");

    if (session.capture.info.sampleRate != session.render.info.sampleRate) {
        std::printf("\n[错误] 麦克风与播放设备采样率不一致（%d vs %d），共享模式下无法直连。\n"
                    "       请在系统声音设置里把两者设为相同采样率后重试。\n",
                    session.capture.info.sampleRate, session.render.info.sampleRate);
        closeEndpoint(session.capture);
        closeEndpoint(session.render);
        safeRelease(session.enumerator);
        CoUninitialize();
        return 1;
    }

    // 环形缓冲：0.5 秒容量，足够吸收调度抖动。
    LiveState state;
    state.sampleRate = session.capture.info.sampleRate;
    state.ring.resize(static_cast<std::size_t>(state.sampleRate) / 2);

    std::printf("\n[2/4] 启动渲染与采集线程（音调 %.3f，FFT %d，采样率 %d）...\n", pitch, fftSize,
                state.sampleRate);

    // 关闭控制台"快速编辑模式"：
    // 该模式下只要用户用鼠标在窗口里点一下，控制台就进入"选择"状态，
    // 所有读写 stdin/stdout 的线程被系统挂起，表现为"程序突然不响了"。
    {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD inMode = 0;
        if (hIn != INVALID_HANDLE_VALUE && GetConsoleMode(hIn, &inMode)) {
            inMode &= ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE);
            inMode |= ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(hIn, inMode);
        }
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    // 先启动采集，等它真正产出第一块音频后再启动播放。
    // （麦克风（尤其阵列麦克风）从 Start 到出第一个包常有数百毫秒，
    //   若播放端同时启动就会连续欠载并输出静音。）
    std::thread capturer(captureLoop, &session, &state, fftSize, pitch);
    for (int i = 0; i < 400 && !state.primed.load(std::memory_order_acquire); ++i) {
        if (g_ctrlStop) break;
        Sleep(10);
    }
    std::thread renderer(renderLoop, &session, &state);

    std::printf("\n[3/4] 已开始实时变声。\n");
    std::printf("      戴上耳机，对着笔记本麦克风说话即可听到变声后的声音。\n");
    std::printf("      在下面输入数字并回车可切换音调（如 1.5 / 0.7 / 2.0），输入 q 退出。\n\n");

    // 主线程：读控制台指令。
    std::vector<vc::PitchShifter*> retired;
    char line[128];
    while (!g_ctrlStop) {
        if (!std::fgets(line, sizeof(line), stdin)) break;

        // 去掉换行。
        std::string cmd(line);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
            cmd.pop_back();
        }
        if (cmd.empty()) continue;
        if (cmd == "q" || cmd == "Q" || cmd == "quit" || cmd == "exit") break;
        if (cmd == "s" || cmd == "S") {
            printOneLineStatus(state);
            continue;
        }

        char* end = nullptr;
        const double v = std::strtod(cmd.c_str(), &end);
        if (!end || end == cmd.c_str() || !(v > 0.05 && v < 8.0)) {
            std::printf("    无效输入。请输入 0.05 ~ 8.0 之间的倍数，或 q 退出。\n");
            continue;
        }

        // 在非实时线程构造新实例，再投递给处理线程切换。
        auto* fresh = new (std::nothrow) vc::PitchShifter(static_cast<float>(state.sampleRate),
                                                          static_cast<float>(v), fftSize);
        if (!fresh) {
            std::printf("    内存不足，切换失败。\n");
            continue;
        }
        vc::PitchShifter* prev = state.pending.exchange(fresh);
        if (prev) retired.push_back(prev);  // 上一请求未被消费，保留待释放
        std::printf("    已请求切换到音调 %.3f（约 50 ms 内淡入生效）\n", v);
    }

    std::printf("\n[4/4] 正在停止...\n");
    state.stop.store(true, std::memory_order_relaxed);
    // 唤醒可能正在等待事件的线程：事件句柄在两秒内必然因设备缓冲刷新而触发，这里直接等待。
    if (capturer.joinable()) capturer.join();
    if (renderer.joinable()) renderer.join();

    printOneLineStatus(state);

    // 释放所有 PitchShifter 实例。
    if (vc::PitchShifter* p = state.active.exchange(nullptr)) retired.push_back(p);
    if (vc::PitchShifter* p = state.pending.exchange(nullptr)) retired.push_back(p);
    for (vc::PitchShifter* p : retired) delete p;

    closeEndpoint(session.capture);
    closeEndpoint(session.render);
    safeRelease(session.enumerator);
    CoUninitialize();

    std::printf("已退出。\n");
    return 0;
}
