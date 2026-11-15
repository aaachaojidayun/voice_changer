#include "vc/fft.hpp"

#include <cmath>
#include <utility>

namespace vc {

namespace {
constexpr float kTwoPi = 6.283185307179586476925287f;
}  // namespace

RealFft::RealFft(int n) : n_(n) {
    // 只接受 2 的幂；调用方已校验，这里兜底修正到最近的上界。
    int p = 1;
    log2n_ = 0;
    while (p < n_) {
        p <<= 1;
        ++log2n_;
    }
    n_ = p;

    rev_.resize(static_cast<std::size_t>(n_));
    for (int i = 0; i < n_; ++i) {
        int r = 0;
        for (int b = 0; b < log2n_; ++b) {
            if (i & (1 << b)) r |= 1 << (log2n_ - 1 - b);
        }
        rev_[static_cast<std::size_t>(i)] = r;
    }

    // 旋转因子表：tw[k] = exp(-j*2*pi*k/n)，只需 n/2 个
    tw_re_.resize(static_cast<std::size_t>(n_ / 2));
    tw_im_.resize(static_cast<std::size_t>(n_ / 2));
    for (int k = 0; k < n_ / 2; ++k) {
        const float ang = kTwoPi * static_cast<float>(k) / static_cast<float>(n_);
        tw_re_[static_cast<std::size_t>(k)] = std::cos(ang);
        tw_im_[static_cast<std::size_t>(k)] = std::sin(ang);
    }

    buf_re_.assign(static_cast<std::size_t>(n_), 0.0f);
    buf_im_.assign(static_cast<std::size_t>(n_), 0.0f);
}

void RealFft::transform() const {
    const int n = n_;

    // 位反转置换
    for (int i = 0; i < n; ++i) {
        const int j = rev_[static_cast<std::size_t>(i)];
        if (j > i) {
            std::swap(buf_re_[static_cast<std::size_t>(i)], buf_re_[static_cast<std::size_t>(j)]);
            std::swap(buf_im_[static_cast<std::size_t>(i)], buf_im_[static_cast<std::size_t>(j)]);
        }
    }

    // 迭代蝶形（DIT）
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int base = 0; base < n; base += len) {
            int k = 0;
            for (int j = 0; j < half; ++j, k += step) {
                const float wr = tw_re_[static_cast<std::size_t>(k)];
                const float wi = -tw_im_[static_cast<std::size_t>(k)];  // 正向：e^{-j...}
                const int a = base + j;
                const int b = a + half;
                const float xr = buf_re_[static_cast<std::size_t>(b)] * wr -
                                 buf_im_[static_cast<std::size_t>(b)] * wi;
                const float xi = buf_re_[static_cast<std::size_t>(b)] * wi +
                                 buf_im_[static_cast<std::size_t>(b)] * wr;
                buf_re_[static_cast<std::size_t>(b)] =
                    buf_re_[static_cast<std::size_t>(a)] - xr;
                buf_im_[static_cast<std::size_t>(b)] =
                    buf_im_[static_cast<std::size_t>(a)] - xi;
                buf_re_[static_cast<std::size_t>(a)] += xr;
                buf_im_[static_cast<std::size_t>(a)] += xi;
            }
        }
    }
}

void RealFft::forward(const float* time, float* freq_re, float* freq_im) const {
    const int n = n_;
    for (int i = 0; i < n; ++i) {
        buf_re_[static_cast<std::size_t>(i)] = time[i];
        buf_im_[static_cast<std::size_t>(i)] = 0.0f;
    }
    transform();
    for (int k = 0; k <= n / 2; ++k) {
        freq_re[k] = buf_re_[static_cast<std::size_t>(k)];
        freq_im[k] = buf_im_[static_cast<std::size_t>(k)];
    }
}

void RealFft::inverse(const float* freq_re, const float* freq_im, float* time) const {
    const int n = n_;

    // 由半谱重建共轭对称的完整谱：X[n-k] = conj(X[k])
    for (int k = 0; k <= n / 2; ++k) {
        buf_re_[static_cast<std::size_t>(k)] = freq_re[k];
        buf_im_[static_cast<std::size_t>(k)] = freq_im[k];
    }
    for (int k = n / 2 + 1; k < n; ++k) {
        const int m = n - k;
        buf_re_[static_cast<std::size_t>(k)] = freq_re[m];
        buf_im_[static_cast<std::size_t>(k)] = -freq_im[m];
    }

    // 逆变换 = conj( FFT( conj(X) ) ) / n
    for (int i = 0; i < n; ++i) {
        buf_im_[static_cast<std::size_t>(i)] = -buf_im_[static_cast<std::size_t>(i)];
    }
    transform();
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        time[i] = buf_re_[static_cast<std::size_t>(i)] * inv;
    }
}

}  // namespace vc
