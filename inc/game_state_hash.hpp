#pragma once

#include "game_state.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace benzen {

namespace detail {

inline void hashCombine(std::size_t& h, std::size_t value) noexcept {
    // 64 bit / 32 bit のどちらでも使用できる一般的な combine。
    h ^= value + static_cast<std::size_t>(0x9e3779b9U) +
         (h << 6U) + (h >> 2U);
}

template <class T>
inline void hashValue(std::size_t& h, T value) noexcept {
    hashCombine(h, std::hash<T>{}(value));
}

inline bool equalAgent(const Agent& lhs, const Agent& rhs) noexcept {
    return lhs.index == rhs.index && lhs.kind == rhs.kind &&
           lhs.pos == rhs.pos && lhs.fuel == rhs.fuel;
}

inline bool equalPendingAction(
    const PendingAction& lhs,
    const PendingAction& rhs) noexcept {
    return lhs.active == rhs.active && lhs.move == rhs.move &&
           lhs.rawAction == rhs.rawAction &&
           lhs.remainingSteps == rhs.remainingSteps &&
           lhs.from == rhs.from && lhs.destination == rhs.destination &&
           lhs.fuelCost == rhs.fuelCost;
}

}  // namespace detail

// 得点も含めた完全な探索状態の比較。
struct GameStateEqual {
    bool operator()(const GameState& lhs, const GameState& rhs) const noexcept {
        if (lhs.day != rhs.day || lhs.step != rhs.step ||
            lhs.dayStepLimit != rhs.dayStepLimit ||
            lhs.agentCount != rhs.agentCount ||
            lhs.spotCount != rhs.spotCount ||
            lhs.dayBrands != rhs.dayBrands || lhs.dayUdon != rhs.dayUdon ||
            lhs.score.matchBrands != rhs.score.matchBrands ||
            lhs.score.totalUdon != rhs.score.totalUdon ||
            lhs.score.sumOfDailyBrandCounts !=
                rhs.score.sumOfDailyBrandCounts ||
            lhs.finalized != rhs.finalized) {
            return false;
        }

        for (std::size_t i = 0; i < lhs.agentCount; ++i) {
            if (!detail::equalAgent(lhs.agents[i], rhs.agents[i]) ||
                !detail::equalPendingAction(
                    lhs.pendingActions[i], rhs.pendingActions[i]) ||
                lhs.acquiredSpots[i] != rhs.acquiredSpots[i]) {
                return false;
            }
        }
        for (std::size_t i = 0; i < lhs.spotCount; ++i) {
            if (lhs.remainingStocks[i] != rhs.remainingStocks[i]) {
                return false;
            }
        }
        return true;
    }
};

struct GameStateHash {
    std::size_t operator()(const GameState& st) const noexcept {
        std::size_t h = 0;
        // 32bit値をロスなく束ね、依存するhashCombineの回数を減らす。
        // パディングや未使用配列を読まず、完全比較と同じフィールドを使う。
        const auto pair = [&](std::int32_t lo, std::int32_t hi) {
            detail::hashValue(h, static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(lo)) |
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32));
        };
        pair(st.day, st.step);
        detail::hashValue(h, st.dayStepLimit);
        detail::hashValue(h, st.agentCount);
        detail::hashValue(h, st.spotCount);
        for (std::size_t i = 0; i < st.agentCount; ++i) {
            const Agent& a = st.agents[i];
            const PendingAction& p = st.pendingActions[i];
            detail::hashValue(h, a.index);
            const auto flags = static_cast<std::uint32_t>(a.kind) |
                (static_cast<std::uint32_t>(p.active) << 8) |
                (static_cast<std::uint32_t>(p.move) << 9);
            pair(static_cast<std::int32_t>(flags), p.fuelCost);
            pair(a.pos, a.fuel);
            pair(p.rawAction, p.remainingSteps);
            pair(p.from, p.destination);
            detail::hashValue(h, st.acquiredSpots[i]);
        }
        std::size_t i = 0;
        for (; i + 1 < st.spotCount; i += 2)
            pair(st.remainingStocks[i], st.remainingStocks[i + 1]);
        if (i < st.spotCount) detail::hashValue(h, st.remainingStocks[i]);
        detail::hashValue(h, st.dayBrands);
        pair(st.dayUdon, st.score.totalUdon);
        detail::hashValue(h, st.score.matchBrands);
        pair(st.score.sumOfDailyBrandCounts, st.finalized ? 1 : 0);
        return h;
    }
};

inline bool operator==(const GameState& lhs, const GameState& rhs) noexcept {
    return GameStateEqual{}(lhs, rhs);
}

inline bool operator!=(const GameState& lhs, const GameState& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace benzen
