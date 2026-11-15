// 定长环形缓冲。容量在 resize() 时一次确定，运行期不再分配。
// 用于实时路径上的输入/输出队列，保证 process() 内部零 malloc。
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace vc {

class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity = 1) { resize(capacity); }

    void resize(std::size_t capacity) {
        cap_ = capacity ? capacity : 1;
        buf_.assign(cap_, 0.0f);
        head_ = 0;
        size_ = 0;
    }

    void clear() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        head_ = 0;
        size_ = 0;
    }

    std::size_t capacity() const { return cap_; }
    std::size_t size() const { return size_; }
    std::size_t freeSpace() const { return cap_ - size_; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ == cap_; }

    // 压入一个样本。已满时丢弃最旧样本，保证不会越界写。
    void push(float v) {
        if (size_ == cap_) {
            buf_[head_] = v;
            head_ = (head_ + 1) % cap_;
            return;
        }
        buf_[(head_ + size_) % cap_] = v;
        ++size_;
    }

    void pushN(const float* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) push(data[i]);
    }

    // 读取第 i 个样本（0 为最旧），不弹出，不检查 i < size()。
    float at(std::size_t i) const { return buf_[(head_ + i) % cap_]; }

    float pop() {
        const float v = buf_[head_];
        head_ = (head_ + 1) % cap_;
        --size_;
        return v;
    }

    std::size_t popN(float* out, std::size_t n) {
        const std::size_t k = (n < size_) ? n : size_;
        for (std::size_t i = 0; i < k; ++i) out[i] = pop();
        return k;
    }

    void skip(std::size_t n) {
        const std::size_t k = (n < size_) ? n : size_;
        head_ = (head_ + k) % cap_;
        size_ -= k;
    }

private:
    std::vector<float> buf_;
    std::size_t cap_ = 1;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

}  // namespace vc
