// Phase Vocoder 时间伸缩核心。
//
// 三件事：STFT 分析、相位累积修正、Overlap-Add 合成加归一化。
// 单独拆出来，是为了让"时间伸缩"与"重采样"两个概念在代码上分离，
// 也让 phase_vocoder 可以脱离 PitchShifter 用于纯时间伸缩场景。
#pragma once

#include <vector>

#include "vc/fft.hpp"

namespace vc {

class PhaseVocoder {
public:
    PhaseVocoder(int fftSize, int analHop, int synthHop);

    void reset();

    // 输入 fftSize() 个分析帧样本，输出 synthHop() 个已归一化的合成样本。
    void processFrame(const float* frame, float* out);

    int fftSize() const { return n_; }
    int analHop() const { return ha_; }
    int synthHop() const { return hs_; }
    int bins() const { return nb_; }

private:
    int n_;
    int nb_;
    int ha_;
    int hs_;

    std::vector<float> window_;   // 周期 Hann 窗
    std::vector<float> omega_;    // 标称 bin 角频率，单位 rad/sample

    std::vector<float> ana_;      // 分析时域帧
    std::vector<float> syn_;      // 合成时域帧
    std::vector<float> specRe_;   // 当前谱实部
    std::vector<float> specIm_;   // 当前谱虚部

    std::vector<float> prevPhase_;  // 上一帧的分析相位 φ_{m-1}
    std::vector<float> sumPhase_;   // 累积的合成相位 ψ_m

    std::vector<float> ola_;   // OLA 累加缓冲（环形，长度 n_）
    std::vector<float> norm_;  // 窗平方和的累加缓冲（环形，长度 n_）
    int pos_ = 0;              // 当前帧在环形缓冲中的起点

    RealFft fft_;
};

}  // namespace vc
