// 实数输入 radix-2 FFT。
//
// 说明：本机离线，无法拉取 kissfft 源码，故内置一份等价实现。
// 接口刻意对齐 kissfft 的 kiss_fftr 使用方式（实数输入、只暴露 N/2+1 个 bin），
// 未来联网后接入 kissfft 只需替换本文件的实现，调用方无需改动。
//
// 约束：n 必须是 2 的幂；所有工作缓冲在构造期分配，forward/inverse 内零分配。
#pragma once

#include <vector>

namespace vc {

class RealFft {
public:
    explicit RealFft(int n);

    int size() const { return n_; }
    int bins() const { return n_ / 2 + 1; }

    // time: n 个实数样本
    // freq_re / freq_im: bins 个复数分量
    void forward(const float* time, float* freq_re, float* freq_im) const;

    // freq_re / freq_im: bins 个复数分量
    // time: n 个实数样本（已除以 n，即标准归一化逆变换）
    void inverse(const float* freq_re, const float* freq_im, float* time) const;

private:
    void transform() const;  // 原地正向 FFT，作用在 buf_re_/buf_im_ 上

    int n_;
    int log2n_;
    std::vector<int> rev_;
    std::vector<float> tw_re_;
    std::vector<float> tw_im_;
    mutable std::vector<float> buf_re_;
    mutable std::vector<float> buf_im_;
};

}  // namespace vc
