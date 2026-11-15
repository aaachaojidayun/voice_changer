// 三次插值（Catmull-Rom）重采样器。
//
// 变调链路里负责"把时间伸缩后的信号按步长 step 重新采样"，
// 使时长回到 1/step 倍、频率乘 step 倍。step 即 --pitch 的 α。
//
// 流式实现：跨调用保持 4 点插值历史与小数相位 frac，不做块内重启。
// 选三次而非线性的原因：线性插值在高频段有明显衰减与镜像失真。
#pragma once

#include <cstddef>

#include "vc/ring_buffer.hpp"

namespace vc {

class Resampler {
public:
    explicit Resampler(float step = 1.0f, std::size_t capacity = 4096);

    void reset();
    void setStep(float step) { step_ = step; }
    float step() const { return step_; }

    // 写入待重采样的输入样本。
    void push(const float* in, std::size_t n);

    // 从已缓冲输入中产出，最多 cap 个，返回实际产出数。
    std::size_t pull(float* out, std::size_t cap);

    // 用零延拓把残余排空（仅在离线收尾时使用）。
    std::size_t drain(float* out, std::size_t cap);

    std::size_t buffered() const { return in_.size(); }

private:
    float interp(float t) const;
    void shift(float next);

    RingBuffer in_;
    float h_[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // x[i-1], x[i], x[i+1], x[i+2]
    float frac_ = 0.0f;                       // 当前插值点小数部分，属于 [0, 1)
    bool started_ = false;
    float step_ = 1.0f;
};

}  // namespace vc
