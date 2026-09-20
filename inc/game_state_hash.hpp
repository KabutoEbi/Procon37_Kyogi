#pragma once

#include "game_state.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace procon37 {

namespace detail {

inline void hashCombine(std::size_t& seed, std::size_t value) noexcept {
    // 64 bit / 32 bit のどちらでも使用できる一般的な combine。
    seed ^= value + static_cast<std::size_t>(0x9e3779b9U) +
            (seed << 6U) + (seed >> 2U);
}

template <class T>
inline void hashValue(std::size_t& seed, T value) noexcept {
    hashCombine(seed, std::hash<T>{}(value));
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
    std::size_t operator()(const GameState& state) const noexcept {
        std::size_t seed = 0;
        detail::hashValue(seed, state.day);
        detail::hashValue(seed, state.step);
        detail::hashValue(seed, state.dayStepLimit);
        detail::hashValue(seed, state.agentCount);
        detail::hashValue(seed, state.spotCount);

        for (std::size_t i = 0; i < state.agentCount; ++i) {
            const Agent& agent = state.agents[i];
            detail::hashValue(seed, agent.index);
            detail::hashValue(seed, static_cast<std::uint8_t>(agent.kind));
            detail::hashValue(seed, agent.pos);
            detail::hashValue(seed, agent.fuel);

            const PendingAction& pending = state.pendingActions[i];
            detail::hashValue(seed, pending.active);
            detail::hashValue(seed, pending.move);
            detail::hashValue(seed, pending.rawAction);
            detail::hashValue(seed, pending.remainingSteps);
            detail::hashValue(seed, pending.from);
            detail::hashValue(seed, pending.destination);
            detail::hashValue(seed, pending.fuelCost);
            detail::hashValue(seed, state.acquiredSpots[i]);
        }
        for (std::size_t i = 0; i < state.spotCount; ++i) {
            detail::hashValue(seed, state.remainingStocks[i]);
        }

        detail::hashValue(seed, state.dayBrands);
        detail::hashValue(seed, state.dayUdon);
        detail::hashValue(seed, state.score.matchBrands);
        detail::hashValue(seed, state.score.totalUdon);
        detail::hashValue(seed, state.score.sumOfDailyBrandCounts);
        detail::hashValue(seed, state.finalized);
        return seed;
    }
};

inline bool operator==(const GameState& lhs, const GameState& rhs) noexcept {
    return GameStateEqual{}(lhs, rhs);
}

inline bool operator!=(const GameState& lhs, const GameState& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace procon37
