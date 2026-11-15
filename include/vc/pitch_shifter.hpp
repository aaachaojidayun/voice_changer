// 变调（保持时长）对外唯一入口。
//
// 链路：输入 -> PhaseVocoder 时间伸缩(约 alpha 倍长) -> Resampler 重采样(步长 alpha)
//       -> 输出。伸缩让时长变长 alpha 倍，重采样让时长回到 1/alpha 倍、频率乘 alpha，
//       两者复合的结果就是"时长不变、音高乘以 alpha"。
//
// 接口语义：
//   process(in, out, frames) 严格等长输入输出。链路内部存在固定延迟 latency()，
//   即 out[k] 实际对应输入的第 k - latency() 个样本。离线处理时按如下方式恢复对齐：
//       process 若干块 -> flush() -> readTail() 收集尾段 -> 整体从 latency() 处取 L 个样本。
//
// 所有缓冲在构造期分配，process() 内不做任何动态分配。
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "vc/phase_vocoder.hpp"
#include "vc/resampler.hpp"
#include "vc/ring_buffer.hpp"

namespace vc {

class PitchShifter {
public:
    PitchShifter(float sampleRate, float pitch, int fftSize = 1024);

    // 严格等长：读入 frames 个，写出 frames 个。
    void process(const float* in, float* out, std::size_t frames);

    // 固定延迟（样本数）。离线恢复对齐时需裁掉输出开头的这么多个样本。
    std::size_t latency() const { return latency_; }

    // 送入静音把内部管线排空，之后用 readTail() 取残余。
    void flush();

    // 取排空后的残余样本，返回实际取出个数。
    std::size_t readTail(float* out, std::size_t maxFrames);

    void reset();

    float pitch() const { return pitch_; }
    int fftSize() const { return fftSize_; }
    int analHop() const { return hopA_; }
    int synthHop() const { return hopS_; }

private:
    // 生产到输出队列至少有 need 个样本，或无法继续生产为止。
    void pump(std::size_t need);
    bool produceFrame();

    int fftSize_;
    int hopA_;
    int hopS_;
    float pitch_;
    std::size_t latency_;
    bool flushed_ = false;

    std::unique_ptr<PhaseVocoder> pv_;
    Resampler rs_;

    RingBuffer in_;
    RingBuffer out_;

    std::vector<float> frameBuf_;
    std::vector<float> scratch_;
};

}  // namespace vc
