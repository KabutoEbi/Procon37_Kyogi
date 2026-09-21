#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace procon37 {

// 単調な距離と最大増分4のDialキュー。平地2、山3、道路1/2/4に対応。
template <class T, std::size_t MaxEdge = 4>
class DialQueue {
public:
    bool empty() const noexcept { return size_ == 0; }

    void push(std::int32_t distance, T value) {
        assert(distance >= current_ &&
               distance <= current_ + static_cast<std::int32_t>(MaxEdge));
        buckets_[static_cast<std::size_t>(distance) % buckets_.size()]
            .emplace_back(distance, std::move(value));
        ++size_;
    }

    std::pair<std::int32_t, T> pop() {
        assert(!empty());
        while (buckets_[static_cast<std::size_t>(current_) % buckets_.size()]
                   .empty()) {
            ++current_;
        }
        auto& bucket = buckets_[static_cast<std::size_t>(current_) % buckets_.size()];
        auto entry = std::move(bucket.front());
        bucket.pop_front();
        --size_;
        assert(entry.first == current_);
        return entry;
    }

private:
    std::array<std::deque<std::pair<std::int32_t, T>>, MaxEdge + 1> buckets_;
    std::int32_t current_ = 0;
    std::size_t size_ = 0;
};

}  // namespace procon37
