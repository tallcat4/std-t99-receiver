#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

namespace t99 {
// Bounded queue between the USB callback and DSP. Never silently splice IQ
// across a dropped buffer: an overrun terminates capture with an explicit error.
class IqQueue {
    std::vector<unsigned char> data_;
    std::size_t head_ = 0, size_ = 0;
    std::mutex mutex_;
    std::condition_variable ready_;
    enum class Error { none, overflow, invalid, device } error_ = Error::none;
    bool done_ = false;
public:
    explicit IqQueue(std::size_t capacity = 16 * 1024 * 1024) : data_(capacity) {
        if (!capacity || capacity % 2) throw std::invalid_argument("IQ queue capacity must be positive and even");
    }
    bool push(std::span<const unsigned char> bytes) {
        std::lock_guard lock(mutex_);
        if (done_ || error_ != Error::none) return false;
        if (bytes.empty() || bytes.size() % 2) error_ = Error::invalid;
        else if (bytes.size() > data_.size() - size_) error_ = Error::overflow;
        else {
            const auto tail = (head_ + size_) % data_.size();
            const auto first = std::min(bytes.size(), data_.size() - tail);
            std::memcpy(data_.data() + tail, bytes.data(), first);
            std::memcpy(data_.data(), bytes.data() + first, bytes.size() - first);
            size_ += bytes.size();
        }
        ready_.notify_one();
        return error_ == Error::none;
    }
    void finish(bool failed) {
        std::lock_guard lock(mutex_);
        done_ = true;
        if (failed && error_ == Error::none) error_ = Error::device;
        ready_.notify_all();
    }
    std::size_t pop(std::span<unsigned char> bytes, bool (*stop_requested)() = nullptr) {
        if (bytes.empty() || bytes.size() % 2) throw std::invalid_argument("IQ read size must be positive and even");
        std::unique_lock lock(mutex_);
        while (!size_ && !done_ && error_ == Error::none) {
            if (stop_requested && stop_requested()) return 0;
            ready_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (stop_requested && stop_requested()) return 0;
        switch (error_) {
        case Error::overflow: throw std::runtime_error("RTL-SDR IQ queue overflow: capture stopped because the consumer cannot keep up");
        case Error::invalid: throw std::runtime_error("Invalid RTL-SDR USB buffer length");
        case Error::device: throw std::runtime_error("RTL-SDR asynchronous read stopped unexpectedly (USB disconnected?)");
        case Error::none: break;
        }
        const auto count = std::min(bytes.size(), size_);
        const auto first = std::min(count, data_.size() - head_);
        std::memcpy(bytes.data(), data_.data() + head_, first);
        std::memcpy(bytes.data() + first, data_.data(), count - first);
        head_ = (head_ + count) % data_.size();
        size_ -= count;
        return count;
    }
};
}
