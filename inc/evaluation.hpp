#pragma once

#include "game_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace procon37 {

struct EvaluationWeights {
    // 既定値は公式の辞書順（試合系列 > 日別系列 > 玉数）を強く優先する。
    double newBrand = 1.0e12;
    double dailyBrand = 1.0e8;
    double udon = 1.0e4;
    double remainingFuel = 1.0;
    double unusedStep = -0.5;
    double valuableSpotDistance = -2.0;
    double supplyDistance = -1.0;
    double concentration = -10.0;
};

// 距離値は評価直前にダイクストラ結果から設定する。
struct EvaluationContext {
    ScoreState scoreAtDayStart{};
    std::array<std::int32_t, kMaxAgents> nearestValuableSpotSteps{};
    std::array<std::int32_t, kMaxAgents> nearestSupplySteps{};

    // 複数台が同じ低価値目標へ向かう等の、探索側で計算する競合度。
    std::int32_t concentrationCount = 0;

    EvaluationContext() noexcept {
        nearestValuableSpotSteps.fill(kImpassableCost);
        nearestSupplySteps.fill(kImpassableCost);
    }
};

struct EvaluationBreakdown {
    std::int32_t newBrandCount = 0;
    std::int32_t dailyBrandCount = 0;
    std::int32_t udonCount = 0;
    std::int32_t remainingFuel = 0;
    std::int32_t unusedSteps = 0;
    std::int32_t valuableSpotDistance = 0;
    std::int32_t supplyDistance = 0;
    std::int32_t concentrationCount = 0;
    double total = 0.0;
};

inline EvaluationBreakdown evaluateGameState(
    const GameState& state,
    const EvaluationContext& context,
    const EvaluationWeights& weights = {}) noexcept {
    EvaluationBreakdown result;
    result.newBrandCount = static_cast<std::int32_t>(countBits(
        state.score.matchBrands & ~context.scoreAtDayStart.matchBrands));
    result.dailyBrandCount =
        static_cast<std::int32_t>(countBits(state.dayBrands));
    result.udonCount =
        state.score.totalUdon - context.scoreAtDayStart.totalUdon;
    result.unusedSteps = state.remainingSteps();
    result.concentrationCount = context.concentrationCount;

    for (std::size_t i = 0; i < state.agentCount; ++i) {
        const Agent& agent = state.agents[i];
        if (agent.isPatrol()) {
            result.remainingFuel += agent.fuel;
            if (context.nearestValuableSpotSteps[i] < kImpassableCost) {
                result.valuableSpotDistance +=
                    context.nearestValuableSpotSteps[i];
            }
            if (context.nearestSupplySteps[i] < kImpassableCost) {
                result.supplyDistance += context.nearestSupplySteps[i];
            }
        }
    }

    result.total =
        weights.newBrand * result.newBrandCount +
        weights.dailyBrand * result.dailyBrandCount +
        weights.udon * result.udonCount +
        weights.remainingFuel * result.remainingFuel +
        weights.unusedStep * result.unusedSteps +
        weights.valuableSpotDistance * result.valuableSpotDistance +
        weights.supplyDistance * result.supplyDistance +
        weights.concentration * result.concentrationCount;
    return result;
}

}  // namespace procon37
