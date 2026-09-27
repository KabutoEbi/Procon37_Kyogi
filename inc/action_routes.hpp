#pragma once

#include "game.hpp"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace benzen {

// 未実行経路のsuffixを共有する。等しい残り方向列は同じIDになる。
class ActionRoutes {
public:
    using Id = std::uint32_t;
    static constexpr Id none = std::numeric_limits<Id>::max();
    struct Entry { std::int32_t action; Id next; };

    Id intern(const AgentPlan& actions) {
        Id tail = none;
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            const std::uint64_t key =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(*it)) << 32) |
                tail;
            const auto found = ids_.find(key);
            if (found != ids_.end()) {
                tail = found->second;
            } else {
                const Id id = static_cast<Id>(entries_.size());
                entries_.push_back({*it, tail});
                ids_.emplace(key, id);
                tail = id;
            }
        }
        return tail;
    }

    Entry operator[](Id id) const { return entries_[id]; }

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::uint64_t, Id> ids_;
};

}  // namespace benzen
