#include "vc/pitch_shifter.hpp"

#include <algorithm>
#include <cmath>

namespace vc {

PitchShifter::PitchShifter(float sampleRate, float pitch, int fftSize)
    : fftSize_(fftSize),
      hopA_(fftSize / 4),
      pitch_(pitch) {
    (void)sampleRate;

    // 合成 hop 取整，保证 OLA 落在整数栅格上。
    // 取整带来的伸缩比偏差由重采样步长补偿（步长仍取精确 alpha）。
    int hs = static_cast<int>(std::lround(static_cast<double>(pitch) *
                                          static_cast<double>(hopA_)));
    hs = std::max(1, hs);
    // OLA 要求一帧内不少于两次重叠，否则归一化退化为 1/hop。
    hs = std::min(hs, fftSize / 2);
    hopS_ = hs;

    pv_ = std::make_unique<PhaseVocoder>(fftSize_, hopA_, hopS_);

    // 重采样步长取精确 alpha：保证 --pitch 1.5 时 1kHz 准确落到 1500Hz。
    rs_ = Resampler(pitch_, static_cast<std::size_t>(fftSize_) * 8);

    in_.resize(static_cast<std::size_t>(fftSize_) * 2);
    out_.resize(static_cast<std::size_t>(fftSize_) * 8);

    frameBuf_.assign(static_cast<std::size_t>(fftSize_), 0.0f);
    scratch_.assign(static_cast<std::size_t>(fftSize_) * 2, 0.0f);

    // 延迟 = 攒满一帧所需样本 + 重采样器起播所需样本，留少量余量。
    latency_ = static_cast<std::size_t>(fftSize_) + 8;
    flushed_ = false;
}

void PitchShifter::reset() {
    if (pv_) pv_->reset();
    rs_.reset();
    in_.clear();
    out_.clear();
    std::fill(frameBuf_.begin(), frameBuf_.end(), 0.0f);
    flushed_ = false;
}

bool PitchShifter::produceFrame() {
    if (in_.size() < static_cast<std::size_t>(fftSize_)) return false;

    // 取当前窗口（不弹出），只前进分析 hop。
    for (int i = 0; i < fftSize_; ++i) {
        frameBuf_[static_cast<std::size_t>(i)] = in_.at(static_cast<std::size_t>(i));
    }
    in_.skip(static_cast<std::size_t>(hopA_));

    // 一帧合成产出 hopS_ 个拉伸样本，推入重采样器。
    pv_->processFrame(frameBuf_.data(), scratch_.data());
    rs_.push(scratch_.data(), static_cast<std::size_t>(hopS_));
    return true;
}

void PitchShifter::pump(std::size_t need) {
    while (out_.size() < need) {
        // 先把重采样器里有的一次抽干
        const std::size_t got = rs_.pull(scratch_.data(), scratch_.size());
        if (got > 0) {
            out_.pushN(scratch_.data(), got);
            continue;
        }
        // 重采样器没料了，说明需要更多拉伸信号
        if (!produceFrame()) break;
    }
}

void PitchShifter::process(const float* in, float* out, std::size_t frames) {
    if (frames == 0) return;

    // 分批推进，每批不超过一个分析 hop，保证内部环形缓冲不会被灌爆。
    std::size_t done = 0;
    const std::size_t chunk = static_cast<std::size_t>(hopA_);
    while (done < frames) {
        const std::size_t n = std::min(chunk, frames - done);
        for (std::size_t i = 0; i < n; ++i) {
            in_.push(in ? in[done + i] : 0.0f);
        }

        pump(n);
        const std::size_t got = out_.popN(out + done, n);
        for (std::size_t i = got; i < n; ++i) {
            out[done + i] = 0.0f;  // 起播延迟段补静音
        }
        done += n;
    }
}

void PitchShifter::flush() {
    if (flushed_) return;
    flushed_ = true;

    const std::size_t want = latency_ + static_cast<std::size_t>(fftSize_);
    const std::size_t chunk = static_cast<std::size_t>(hopA_);

    // 持续喂静音推进管线，把残余信号全部推到 out_ 里
    for (int guard = 0; guard < 128 && out_.size() < want; ++guard) {
        for (std::size_t i = 0; i < chunk; ++i) in_.push(0.0f);

        while (produceFrame()) {
            // 产帧过程中拉伸样本进入重采样器
        }
        std::size_t got = 0;
        while ((got = rs_.pull(scratch_.data(), scratch_.size())) > 0) {
            out_.pushN(scratch_.data(), got);
        }
    }
}

std::size_t PitchShifter::readTail(float* out, std::size_t maxFrames) {
    return out_.popN(out, maxFrames);
}

}  // namespace vc
