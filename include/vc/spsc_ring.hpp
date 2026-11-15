// 单生产者单消费者无锁环形缓冲（float）。
//
// 与 ring_buffer.hpp 的 RingBuffer 区别：RingBuffer 用于单线程内部管线、无原子操作；
// 本类用于实时音频层跨线程传递样本（采集/处理线程写、播放线程读），
// 读写指针均为 std::atomic，采用 acquire/release 语义保证内存可见性，
// 写入端与读取端各自只修改自己的指针，因此无需加锁、不会阻塞音频回调。
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace vc {

class SpscRing {
public:
    void resize(std::size_t capacity) {
        cap_ = capacity ? capacity : 1;
        buf_.assign(cap_, 0.0f);
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

    std::size_t capacity() const { return cap_; }

    // 当前可读样本数。
    std::size_t available() const {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_relaxed);
        return w - r;
    }

    // 当前可写样本数。
    std::size_t freeSpace() const {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return cap_ - (w - r);
    }

    // 写入 n 个样本，返回实际写入个数（空间不足时截断，不阻塞）。
    std::size_t write(const float* data, std::size_t n) {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        const std::size_t free = cap_ - (w - r);
        const std::size_t k = (n < free) ? n : free;
        for (std::size_t i = 0; i < k; ++i) buf_[(w + i) % cap_] = data[i];
        write_.store(w + k, std::memory_order_release);
        return k;
    }

    // 读取 n 个样本，返回实际读取个数（数据不足时截断）。
    std::size_t read(float* out, std::size_t n) {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t avail = w - r;
        const std::size_t k = (n < avail) ? n : avail;
        for (std::size_t i = 0; i < k; ++i) out[i] = buf_[(r + i) % cap_];
        read_.store(r + k, std::memory_order_release);
        return k;
    }

    void clear() {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float> buf_;
    std::size_t cap_ = 1;
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
};

}  // namespace vc
