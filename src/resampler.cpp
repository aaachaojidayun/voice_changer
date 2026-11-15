#include "vc/resampler.hpp"

namespace vc {

Resampler::Resampler(float step, std::size_t capacity) : in_(capacity), step_(step) {
    reset();
}

void Resampler::reset() {
    h_[0] = h_[1] = h_[2] = h_[3] = 0.0f;
    frac_ = 0.0f;
    started_ = false;
    in_.clear();
}

void Resampler::push(const float* in, std::size_t n) { in_.pushN(in, n); }

void Resampler::shift(float next) {
    h_[0] = h_[1];
    h_[1] = h_[2];
    h_[2] = h_[3];
    h_[3] = next;
}

// Catmull-Rom 三次插值，t in [0,1) 位于 h_[1] 与 h_[2] 之间。
float Resampler::interp(float t) const {
    const float x0 = h_[0];
    const float x1 = h_[1];
    const float x2 = h_[2];
    const float x3 = h_[3];
    const float a = -0.5f * x0 + 1.5f * x1 - 1.5f * x2 + 0.5f * x3;
    const float b = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c = -0.5f * x0 + 0.5f * x2;
    return ((a * t + b) * t + c) * t + x1;
}

std::size_t Resampler::pull(float* out, std::size_t cap) {
    std::size_t written = 0;

    if (!started_) {
        if (in_.size() < 3) return 0;
        // x[-1] 用 x[0] 延拓，避免起始点产生虚假边沿
        h_[0] = in_.at(0);
        h_[1] = in_.at(0);
        h_[2] = in_.at(1);
        h_[3] = in_.at(2);
        in_.skip(3);
        frac_ = 0.0f;
        started_ = true;
    }

    // 只有在还有输入可滑动时才产出，保证不引入零填充污染。
    while (written < cap && !in_.empty()) {
        out[written++] = interp(frac_);
        frac_ += step_;
        while (frac_ >= 1.0f) {
            frac_ -= 1.0f;
            shift(in_.empty() ? 0.0f : in_.pop());
        }
    }
    return written;
}

std::size_t Resampler::drain(float* out, std::size_t cap) {
    if (!started_) return 0;
    std::size_t written = 0;
    while (written < cap) {
        out[written++] = interp(frac_);
        frac_ += step_;
        while (frac_ >= 1.0f) {
            frac_ -= 1.0f;
            shift(in_.empty() ? 0.0f : in_.pop());
        }
        if (in_.empty() && h_[0] == 0.0f && h_[1] == 0.0f && h_[2] == 0.0f &&
            h_[3] == 0.0f) {
            break;
        }
    }
    return written;
}

}  // namespace vc
