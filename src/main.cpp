// voice_changer —— 离线变声器（变调、时长不变）
//
// 用法: voice_changer input.wav output.wav --pitch 1.5 [--window 1024]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vc/pitch_shifter.hpp"
#include "vc/wav_io.hpp"

namespace {

void usage() {
    std::printf(
        "voice_changer —— 离线变声器（Phase Vocoder 变调，时长不变）\n"
        "\n"
        "用法:\n"
        "  voice_changer <input.wav> <output.wav> [--pitch <0.5~2.0>] [--window <1024|2048>]\n"
        "\n"
        "参数:\n"
        "  --pitch   变调倍数，默认 1.0。1.5 表示升调 1.5 倍，0.8 表示降调。\n"
        "            有效范围 0.5 ~ 2.0。\n"
        "  --window  FFT 窗长，默认 1024，可选 1024 或 2048（必须是 2 的幂）。\n"
        "\n"
        "示例:\n"
        "  voice_changer in.wav out.wav --pitch 1.5\n");
}

bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

}  // namespace

int main(int argc, char** argv) {
    std::string inputPath;
    std::string outputPath;
    float pitch = 1.0f;
    int windowSize = 1024;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        }
        if (a == "--pitch") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: --pitch 缺少参数\n");
                return 2;
            }
            try {
                pitch = std::stof(argv[++i]);
            } catch (...) {
                std::fprintf(stderr, "错误: --pitch 参数不是合法数字\n");
                return 2;
            }
            continue;
        }
        if (a == "--window") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: --window 缺少参数\n");
                return 2;
            }
            try {
                windowSize = std::stoi(argv[++i]);
            } catch (...) {
                std::fprintf(stderr, "错误: --window 参数不是合法整数\n");
                return 2;
            }
            continue;
        }
        if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "错误: 未知选项 %s\n", a.c_str());
            return 2;
        }
        if (inputPath.empty()) {
            inputPath = a;
        } else if (outputPath.empty()) {
            outputPath = a;
        } else {
            std::fprintf(stderr, "错误: 多余的位置参数 %s\n", a.c_str());
            return 2;
        }
    }

    if (inputPath.empty() || outputPath.empty()) {
        usage();
        return 2;
    }
    if (!(pitch >= 0.5f && pitch <= 2.0f)) {
        std::fprintf(stderr, "错误: --pitch 必须在 0.5 ~ 2.0 之间，当前 %.3f\n",
                     static_cast<double>(pitch));
        return 2;
    }
    if (!isPowerOfTwo(windowSize) || windowSize < 256 || windowSize > 8192) {
        std::fprintf(stderr, "错误: --window 必须是 2 的幂且在 256 ~ 8192 之间\n");
        return 2;
    }

    vc::WavData wav;
    std::string err;
    if (!vc::readWav(inputPath, &wav, &err)) {
        std::fprintf(stderr, "读取失败: %s\n", err.c_str());
        return 1;
    }

    const int channels = wav.channels;
    const std::size_t frames = wav.frames();
    std::printf("输入: %s\n", inputPath.c_str());
    std::printf("  采样率 %u Hz, 声道 %d, %zu 帧 (%.3f 秒)\n", wav.sampleRate, channels,
                frames, wav.durationSeconds());

    if (wav.sampleRate != 44100u && wav.sampleRate != 48000u) {
        std::printf("  注意: 采样率为 %u Hz，本工具主要针对 44100/48000 验证\n",
                    wav.sampleRate);
    }

    std::vector<float> outSamples(frames * static_cast<std::size_t>(channels), 0.0f);
    std::vector<float> planar(frames, 0.0f);

    const std::size_t block = 1024;
    std::vector<float> obuf(block, 0.0f);
    std::vector<float> tailBuf(8192, 0.0f);

    for (int c = 0; c < channels; ++c) {
        // 反交错
        for (std::size_t i = 0; i < frames; ++i) {
            planar[i] = wav.samples[i * static_cast<std::size_t>(channels) +
                                    static_cast<std::size_t>(c)];
        }

        vc::PitchShifter ps(static_cast<float>(wav.sampleRate), pitch, windowSize);
        const std::size_t lat = ps.latency();

        // 累积整段输出（含固定延迟段）
        std::vector<float> acc;
        acc.reserve(frames + lat + static_cast<std::size_t>(windowSize) + 64);

        for (std::size_t pos = 0; pos < frames; pos += block) {
            const std::size_t n = std::min(block, frames - pos);
            ps.process(planar.data() + pos, obuf.data(), n);
            acc.insert(acc.end(), obuf.begin(), obuf.begin() + static_cast<long>(n));
        }

        ps.flush();
        std::size_t got = 0;
        while ((got = ps.readTail(tailBuf.data(), tailBuf.size())) > 0) {
            acc.insert(acc.end(), tailBuf.begin(), tailBuf.begin() + static_cast<long>(got));
        }

        // 裁掉固定延迟段，输出严格 L 帧，保证时长与输入一致
        for (std::size_t i = 0; i < frames; ++i) {
            const std::size_t src = i + lat;
            float v = (src < acc.size()) ? acc[src] : 0.0f;
            if (!std::isfinite(v)) v = 0.0f;
            outSamples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
                v;
        }

        std::printf("声道 %d 处理完成 (pitch=%.3f, N=%d, 延迟 %zu 样本)\n", c,
                    static_cast<double>(pitch), windowSize, lat);
    }

    vc::WavData outWav;
    outWav.sampleRate = wav.sampleRate;
    outWav.channels = wav.channels;
    outWav.samples = std::move(outSamples);

    if (!vc::writeWav(outputPath, outWav, &err)) {
        std::fprintf(stderr, "写出失败: %s\n", err.c_str());
        return 1;
    }

    std::printf("输出: %s\n", outputPath.c_str());
    std::printf("  采样率 %u Hz, 声道 %d, %zu 帧 (%.3f 秒)\n", outWav.sampleRate,
                outWav.channels, outWav.frames(), outWav.durationSeconds());
    std::printf("完成。\n");
    return 0;
}
