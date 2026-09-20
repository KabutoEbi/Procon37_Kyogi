#pragma once

#include "game_state.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace procon37 {

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
        detail::hashValue(h, st.day);
        detail::hashValue(h, st.step);
        detail::hashValue(h, st.dayStepLimit);
        detail::hashValue(h, st.agentCount);
        detail::hashValue(h, st.spotCount);

        for (std::size_t i = 0; i < st.agentCount; ++i) {
            const Agent& a = st.agents[i];
            detail::hashValue(h, a.index);
            detail::hashValue(h, static_cast<std::uint8_t>(a.kind));
            detail::hashValue(h, a.pos);
            detail::hashValue(h, a.fuel);

            const PendingAction& p = st.pendingActions[i];
            detail::hashValue(h, p.active);
            detail::hashValue(h, p.move);
            detail::hashValue(h, p.rawAction);
            detail::hashValue(h, p.remainingSteps);
            detail::hashValue(h, p.from);
            detail::hashValue(h, p.destination);
            detail::hashValue(h, p.fuelCost);
            detail::hashValue(h, st.acquiredSpots[i]);
        }
        for (std::size_t i = 0; i < st.spotCount; ++i) {
            detail::hashValue(h, st.remainingStocks[i]);
        }

        detail::hashValue(h, st.dayBrands);
        detail::hashValue(h, st.dayUdon);
        detail::hashValue(h, st.score.matchBrands);
        detail::hashValue(h, st.score.totalUdon);
        detail::hashValue(h, st.score.sumOfDailyBrandCounts);
        detail::hashValue(h, st.finalized);
        return h;
    }
};

inline bool operator==(const GameState& lhs, const GameState& rhs) noexcept {
    return GameStateEqual{}(lhs, rhs);
}

inline bool operator!=(const GameState& lhs, const GameState& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace procon37
