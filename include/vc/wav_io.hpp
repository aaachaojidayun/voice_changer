// WAV 文件读写。算法层完全不知道文件格式的存在。
//
// 说明：本机离线无法拉取 dr_wav，故内置一份等价的最小实现。
// 接口设计刻意与 dr_wav 的 read/write 用法保持同构（一次性读入整段、整段写出），
// 未来联网后若要换成 dr_wav，只需重写本文件的实现。
//
// 支持：16-bit PCM 与 32-bit IEEE float，单声道 / 立体声，任意采样率。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vc {

struct WavData {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    // 交织排列（interleaved），取值范围 [-1, 1]
    std::vector<float> samples;

    std::size_t frames() const {
        return channels ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
    double durationSeconds() const {
        return sampleRate ? static_cast<double>(frames()) / static_cast<double>(sampleRate)
                          : 0.0;
    }
};

// 失败时返回 false，并把原因写入 err。
bool readWav(const std::string& path, WavData* out, std::string* err);

// 始终写成 16-bit PCM。
bool writeWav(const std::string& path, const WavData& data, std::string* err);

}  // namespace vc
