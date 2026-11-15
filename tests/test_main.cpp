// 单元测试：自写断言 + main 返回码，零外部依赖。
//
// 覆盖：
//   1. 输出时长与输入完全一致
//   2. 输出无 NaN / Inf
//   3. 1kHz 正弦 --pitch 1.5 后主频约 1500Hz
//   4. 输出幅值不越界（不因 OLA 增益失控而削波）
//   5. 立体声两声道独立处理，帧数与采样率保持
//
// 返回码：0 全部通过，1 有失败项。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "vc/fft.hpp"
#include "vc/pitch_shifter.hpp"
#include "vc/wav_io.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& msg, int line) {
    if (cond) {
        ++g_pass;
        std::printf("[PASS] %s\n", msg.c_str());
    } else {
        ++g_fail;
        std::printf("[FAIL] %s  (line %d)\n", msg.c_str(), line);
    }
}

void checkNear(double got, double want, double tol, const std::string& msg, int line) {
    if (std::fabs(got - want) <= tol) {
        ++g_pass;
        std::printf("[PASS] %s  (got %.3f, want %.3f)\n", msg.c_str(), got, want);
    } else {
        ++g_fail;
        std::printf("[FAIL] %s  (got %.3f, want %.3f, tol %.3f, line %d)\n", msg.c_str(), got,
                    want, tol, line);
    }
}

#define CHECK(cond, msg) check((cond), (msg), __LINE__)
#define CHECK_NEAR(got, want, tol, msg) checkNear((got), (want), (tol), (msg), __LINE__)

// 生成正弦测试信号
vc::WavData makeSine(std::uint32_t sampleRate, double seconds, double freq, float amp,
                     int channels) {
    vc::WavData d;
    d.sampleRate = sampleRate;
    d.channels = static_cast<std::uint16_t>(channels);
    const std::size_t frames = static_cast<std::size_t>(seconds * sampleRate);
    d.samples.resize(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sampleRate);
        const float v = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979323846 * freq * t));
        for (int c = 0; c < channels; ++c) {
            d.samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
        }
    }
    return d;
}

// 逐声道调用 PitchShifter，返回与输入等长的输出（已裁掉固定延迟）
std::vector<float> runShifter(const std::vector<float>& in, float sampleRate, float pitch,
                              int window) {
    vc::PitchShifter ps(sampleRate, pitch, window);
    const std::size_t lat = ps.latency();
    const std::size_t frames = in.size();

    std::vector<float> acc;
    acc.reserve(frames + lat + static_cast<std::size_t>(window) + 64);

    const std::size_t block = 1024;
    std::vector<float> obuf(block, 0.0f);
    for (std::size_t pos = 0; pos < frames; pos += block) {
        const std::size_t n = (frames - pos < block) ? (frames - pos) : block;
        ps.process(in.data() + pos, obuf.data(), n);
        acc.insert(acc.end(), obuf.begin(), obuf.begin() + static_cast<long>(n));
    }

    ps.flush();
    std::vector<float> tb(8192, 0.0f);
    std::size_t got = 0;
    while ((got = ps.readTail(tb.data(), tb.size())) > 0) {
        acc.insert(acc.end(), tb.begin(), tb.begin() + static_cast<long>(got));
    }

    std::vector<float> out(frames, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t src = i + lat;
        out[i] = (src < acc.size()) ? acc[src] : 0.0f;
    }
    return out;
}

// 用 FFT + 抛物线插值估计主频
double dominantFreq(const std::vector<float>& x, std::uint32_t sampleRate, std::size_t from,
                    int n) {
    if (x.size() < static_cast<std::size_t>(from) + static_cast<std::size_t>(n)) return 0.0;
    std::vector<float> buf(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        // Hann 窗降低泄漏
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i /
                                              static_cast<double>(n));
        buf[static_cast<std::size_t>(i)] =
            static_cast<float>(x[from + static_cast<std::size_t>(i)] * w);
    }

    vc::RealFft fft(n);
    std::vector<float> re(static_cast<std::size_t>(fft.bins()), 0.0f);
    std::vector<float> im(static_cast<std::size_t>(fft.bins()), 0.0f);
    fft.forward(buf.data(), re.data(), im.data());

    int peak = 1;
    double best = -1.0;
    for (int k = 1; k < fft.bins() - 1; ++k) {
        const double m = std::sqrt(static_cast<double>(re[static_cast<std::size_t>(k)]) *
                                       re[static_cast<std::size_t>(k)] +
                                   static_cast<double>(im[static_cast<std::size_t>(k)]) *
                                       im[static_cast<std::size_t>(k)]);
        if (m > best) {
            best = m;
            peak = k;
        }
    }

    // 抛物线插值细化峰值位置
    const double a = std::sqrt(static_cast<double>(re[static_cast<std::size_t>(peak - 1)]) *
                                   re[static_cast<std::size_t>(peak - 1)] +
                               static_cast<double>(im[static_cast<std::size_t>(peak - 1)]) *
                                   im[static_cast<std::size_t>(peak - 1)]);
    const double b = best;
    const double c = std::sqrt(static_cast<double>(re[static_cast<std::size_t>(peak + 1)]) *
                                   re[static_cast<std::size_t>(peak + 1)] +
                               static_cast<double>(im[static_cast<std::size_t>(peak + 1)]) *
                                   im[static_cast<std::size_t>(peak + 1)]);
    double delta = 0.0;
    const double denom = a - 2.0 * b + c;
    if (std::fabs(denom) > 1e-12) delta = 0.5 * (a - c) / denom;

    return (peak + delta) * static_cast<double>(sampleRate) / static_cast<double>(n);
}

bool allFinite(const std::vector<float>& x) {
    for (float v : x) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

double peakAbs(const std::vector<float>& x) {
    double m = 0.0;
    for (float v : x) {
        const double a = std::fabs(static_cast<double>(v));
        if (a > m) m = a;
    }
    return m;
}

}  // namespace

int main() {
    std::printf("=== voice_changer 单元测试 ===\n\n");

    const std::uint32_t sr = 48000;
    const double srcFreq = 1000.0;

    // ---- 用例 1: 48kHz / 1kHz 正弦 / pitch 1.5 ----
    {
        std::printf("[用例 1] 48kHz 单声道 1kHz 正弦, pitch 1.5\n");
        const int seconds = 3;
        vc::WavData in = makeSine(sr, seconds, srcFreq, 0.5f, 1);
        std::vector<float> out = runShifter(in.samples, static_cast<float>(sr), 1.5f, 1024);

        CHECK(out.size() == in.frames(), "输出帧数与输入一致（时长不变）");
        CHECK(allFinite(out), "输出不含 NaN / Inf");

        const double f = dominantFreq(out, sr, 8192, 16384);
        CHECK_NEAR(f, 1500.0, 60.0, "输出主频约为 1500Hz");

        const double pk = peakAbs(out);
        CHECK(pk > 0.05 && pk <= 1.0, "输出幅值在合理范围且不削波");
        std::printf("       实测主频 %.2f Hz，峰值 %.3f\n\n", f, pk);
    }

    // ---- 用例 2: pitch 0.5 降调 ----
    {
        std::printf("[用例 2] 48kHz 单声道 1kHz 正弦, pitch 0.5\n");
        vc::WavData in = makeSine(sr, 2, srcFreq, 0.5f, 1);
        std::vector<float> out = runShifter(in.samples, static_cast<float>(sr), 0.5f, 1024);

        CHECK(out.size() == in.frames(), "输出帧数与输入一致（时长不变）");
        CHECK(allFinite(out), "输出不含 NaN / Inf");

        const double f = dominantFreq(out, sr, 8192, 16384);
        CHECK_NEAR(f, 500.0, 30.0, "输出主频约为 500Hz");
        std::printf("       实测主频 %.2f Hz\n\n", f);
    }

    // ---- 用例 3: 44.1kHz / 2048 窗 / pitch 2.0 ----
    {
        std::printf("[用例 3] 44.1kHz 单声道 1kHz 正弦, pitch 2.0, N=2048\n");
        const std::uint32_t sr2 = 44100;
        vc::WavData in = makeSine(sr2, 2, srcFreq, 0.5f, 1);
        std::vector<float> out = runShifter(in.samples, static_cast<float>(sr2), 2.0f, 2048);

        CHECK(out.size() == in.frames(), "输出帧数与输入一致（时长不变）");
        CHECK(allFinite(out), "输出不含 NaN / Inf");

        const double f = dominantFreq(out, sr2, 8192, 16384);
        CHECK_NEAR(f, 2000.0, 80.0, "输出主频约为 2000Hz");
        std::printf("       实测主频 %.2f Hz\n\n", f);
    }

    // ---- 用例 4: 立体声 ----
    {
        std::printf("[用例 4] 48kHz 立体声 1kHz 正弦, pitch 1.5\n");
        vc::WavData in = makeSine(sr, 2, srcFreq, 0.4f, 2);

        std::vector<float> left(in.frames(), 0.0f);
        std::vector<float> right(in.frames(), 0.0f);
        for (std::size_t i = 0; i < in.frames(); ++i) {
            left[i] = in.samples[i * 2 + 0];
            right[i] = in.samples[i * 2 + 1];
        }
        std::vector<float> ol = runShifter(left, static_cast<float>(sr), 1.5f, 1024);
        std::vector<float> orr = runShifter(right, static_cast<float>(sr), 1.5f, 1024);

        CHECK(ol.size() == in.frames() && orr.size() == in.frames(),
              "两声道输出帧数均与输入一致");
        CHECK(allFinite(ol) && allFinite(orr), "两声道输出不含 NaN / Inf");

        const double f = dominantFreq(ol, sr, 8192, 16384);
        CHECK_NEAR(f, 1500.0, 60.0, "左声道主频约为 1500Hz");
        std::printf("       实测主频 %.2f Hz\n\n", f);
    }

    // ---- 用例 5: 文件落盘往返，验证 WAV 读写与时长 ----
    {
        std::printf("[用例 5] WAV 读写往返（48kHz 立体声 16-bit）\n");
        vc::WavData in = makeSine(sr, 1, 440.0, 0.5f, 2);
        const std::string tmp = "vc_test_roundtrip.wav";
        std::string err;

        CHECK(vc::writeWav(tmp, in, &err), "写出测试 WAV 成功: " + err);

        vc::WavData back;
        err.clear();
        const bool ok = vc::readWav(tmp, &back, &err);
        CHECK(ok, "读回测试 WAV 成功");
        if (ok) {
            CHECK(back.frames() == in.frames(), "读回帧数与原始一致");
            CHECK(back.sampleRate == in.sampleRate, "读回采样率一致");
            CHECK(back.channels == in.channels, "读回声道数一致");
            double maxErr = 0.0;
            for (std::size_t i = 0; i < back.samples.size(); ++i) {
                const double e =
                    std::fabs(static_cast<double>(back.samples[i]) - in.samples[i]);
                if (e > maxErr) maxErr = e;
            }
            CHECK(maxErr < 1.0 / 32000.0, "16-bit 量化误差在 1 LSB 量级内");
            std::printf("       最大往返误差 %.3e\n", maxErr);
        }
        std::remove(tmp.c_str());
        std::printf("\n");
    }

    std::printf("=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
