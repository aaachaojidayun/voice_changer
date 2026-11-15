#include "vc/phase_vocoder.hpp"

#include <cmath>

namespace vc {

namespace {

constexpr float kTwoPi = 6.283185307179586476925287f;
constexpr float kPi = 3.14159265358979323846f;

// 把相位差折回到 (-pi, pi]，即 2*pi 的整数倍去缠绕。
inline float princarg(float x) {
    return x - kTwoPi * std::floor((x + kPi) / kTwoPi);
}

}  // namespace

PhaseVocoder::PhaseVocoder(int fftSize, int analHop, int synthHop)
    : n_(fftSize),
      nb_(fftSize / 2 + 1),
      ha_(analHop),
      hs_(synthHop),
      fft_(fftSize) {
    window_.resize(static_cast<std::size_t>(n_));
    for (int i = 0; i < n_; ++i) {
        // 周期 Hann：分母用 n 而非 n-1，保证 hop = n/4 时 COLA 严格成立。
        window_[static_cast<std::size_t>(i)] =
            0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) / static_cast<float>(n_));
    }

    omega_.resize(static_cast<std::size_t>(nb_));
    for (int k = 0; k < nb_; ++k) {
        omega_[static_cast<std::size_t>(k)] =
            kTwoPi * static_cast<float>(k) / static_cast<float>(n_);
    }

    ana_.assign(static_cast<std::size_t>(n_), 0.0f);
    syn_.assign(static_cast<std::size_t>(n_), 0.0f);
    specRe_.assign(static_cast<std::size_t>(nb_), 0.0f);
    specIm_.assign(static_cast<std::size_t>(nb_), 0.0f);
    prevPhase_.assign(static_cast<std::size_t>(nb_), 0.0f);
    sumPhase_.assign(static_cast<std::size_t>(nb_), 0.0f);
    ola_.assign(static_cast<std::size_t>(n_), 0.0f);
    norm_.assign(static_cast<std::size_t>(n_), 0.0f);
    pos_ = 0;
}

void PhaseVocoder::reset() {
    std::fill(prevPhase_.begin(), prevPhase_.end(), 0.0f);
    std::fill(sumPhase_.begin(), sumPhase_.end(), 0.0f);
    std::fill(ola_.begin(), ola_.end(), 0.0f);
    std::fill(norm_.begin(), norm_.end(), 0.0f);
    pos_ = 0;
}

void PhaseVocoder::processFrame(const float* frame, float* out) {
    const int n = n_;
    const float ha = static_cast<float>(ha_);
    const float hs = static_cast<float>(hs_);

    // ---- 1. 分析：加窗后做实数 FFT ----
    for (int i = 0; i < n; ++i) {
        ana_[static_cast<std::size_t>(i)] =
            frame[i] * window_[static_cast<std::size_t>(i)];
    }
    fft_.forward(ana_.data(), specRe_.data(), specIm_.data());

    // ---- 2. 相位累积修正 + 频率 bin 修正 ----
    for (int k = 0; k < nb_; ++k) {
        const std::size_t ki = static_cast<std::size_t>(k);
        const float re = specRe_[ki];
        const float im = specIm_[ki];
        const float mag = std::sqrt(re * re + im * im);
        const float phi = std::atan2(im, re);

        // 帧间原始相位差，减去"按标称 bin 频率应走的相位"，剩下的就是
        // 该 bin 内真实频率相对标称频率的偏移量（折回主值区间）。
        const float dphi = princarg((phi - prevPhase_[ki]) - omega_[ki] * ha);

        // 频率 bin 修正：这一 bin 的真实瞬时角频率不是 omega_[k]，
        // 而是 omega_[k] + dphi/ha。用真实频率推进合成相位，
        // 才能让变调后的频率精确落在目标位置，而不是整格跳变。
        const float omegaTrue = omega_[ki] + dphi / ha;

        // 相位累积：合成相位自己累加，而不是直接取分析相位。
        // 直接取分析相位会在帧间产生跳变（因为 hop 改变了），听感是"颗粒感"。
        sumPhase_[ki] = princarg(sumPhase_[ki] + omegaTrue * hs);
        prevPhase_[ki] = phi;

        // 用原始幅度 + 累积相位重建谱：幅度不变说明没有做任何音色重采样，
        // 时间/频率的伸缩全部由相位推进承担。
        specRe_[ki] = mag * std::cos(sumPhase_[ki]);
        specIm_[ki] = mag * std::sin(sumPhase_[ki]);
    }

    // ---- 3. 合成：逆变换回到时域 ----
    fft_.inverse(specRe_.data(), specIm_.data(), syn_.data());

    // ---- 4. Overlap-Add：累加信号，同时累加窗平方和 ----
    for (int i = 0; i < n; ++i) {
        const int idx = (pos_ + i) % n;
        const std::size_t ii = static_cast<std::size_t>(idx);
        const float w = window_[static_cast<std::size_t>(i)];
        ola_[ii] += syn_[static_cast<std::size_t>(i)] * w;
        norm_[ii] += w * w;
    }

    // ---- 5. 取出本 hop 已完成段并归一化 ----
    // 此刻 pos_..pos_+hs-1 这些位置已被所有覆盖它的帧累加完毕，
    // 除以窗平方和即可还原平坦增益，避免以 hop 为周期的幅度起伏（爆音）。
    for (int i = 0; i < hs_; ++i) {
        const int idx = (pos_ + i) % n;
        const std::size_t ii = static_cast<std::size_t>(idx);
        const float nz = norm_[ii];
        out[i] = (nz > 1e-8f) ? (ola_[ii] / nz) : 0.0f;
        // 该位置此后不会再被后续帧覆盖（后续帧的窗已完全越过它），清零复用。
        ola_[ii] = 0.0f;
        norm_[ii] = 0.0f;
    }

    pos_ = (pos_ + hs_) % n;
}

}  // namespace vc
