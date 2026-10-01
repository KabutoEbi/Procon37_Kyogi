#pragma once

#include "game_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace benzen {

struct EvaluationWeights {
    // 既定値は公式の辞書順（試合系列 > 日別系列 > 玉数）を強く優先する。
    double newBrand = 1.0e12;
    double dailyBrand = 1.0e8;
    double udon = 1.0e4;
    double remainingFuel = 1.0;
    double unusedStep = -0.5;
    double valuableSpotDistance = -4.0;
    double newBrandSpotDistance = -2000.0;
    double dailyBrandSpotDistance = -200.0;
    double targetSlack = 0.5;
    double supplyDistance = -6.0;
    double rendezvousMismatch = -4.0;
    double fuelRisk = -1000.0;
    double unreachableTarget = -5000.0;
    double concentration = -250.0;
    // 中間日の終端でスポット上にいることによる翌日step 1の確定取得。
    double nextDayNewBrand = 2.5e11;
    double nextDayBrand = 2.5e7;
    double nextDayUdon = 2.5e3;
    double nextDayNewBrandApproach = 1.0e5;
    double nextDayBrandApproach = 1.0e3;
};

// スポット距離と補給合流指標は探索側で設定する。
struct EvaluationContext {
    ScoreState scoreAtDayStart{};
    std::array<std::int32_t, kMaxAgents> nearestValuableSpotSteps{};
    std::array<std::int32_t, kMaxAgents> nearestNewBrandSpotSteps{};
    std::array<std::int32_t, kMaxAgents> nearestDailyBrandSpotSteps{};
    std::array<std::int32_t, kMaxAgents> nearestSupplySteps{};

    // 複数台が同じ低価値目標へ向かう等の、探索側で計算する競合度。
    std::int32_t concentrationCount = 0;
    std::int32_t targetSlack = 0;
    std::int32_t rendezvousMismatch = 0;
    std::int32_t fuelRisk = 0;
    std::int32_t unreachableTargetCount = 0;
    std::int32_t nextDayNewBrandCount = 0;
    std::int32_t nextDayBrandCount = 0;
    std::int32_t nextDayUdonCount = 0;
    std::int32_t nextDayNewBrandApproach = 0;
    std::int32_t nextDayBrandApproach = 0;

    EvaluationContext() noexcept {
        nearestValuableSpotSteps.fill(kImpassableCost);
        nearestNewBrandSpotSteps.fill(kImpassableCost);
        nearestDailyBrandSpotSteps.fill(kImpassableCost);
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
    std::int32_t newBrandSpotDistance = 0;
    std::int32_t dailyBrandSpotDistance = 0;
    std::int32_t supplyDistance = 0;
    std::int32_t concentrationCount = 0;
    std::int32_t targetSlack = 0;
    std::int32_t rendezvousMismatch = 0;
    std::int32_t fuelRisk = 0;
    std::int32_t unreachableTargetCount = 0;
    std::int32_t nextDayNewBrandCount = 0;
    std::int32_t nextDayBrandCount = 0;
    std::int32_t nextDayUdonCount = 0;
    std::int32_t nextDayNewBrandApproach = 0;
    std::int32_t nextDayBrandApproach = 0;
    std::int32_t valuableSpotAgentCount = 0;
    std::int32_t newBrandSpotAgentCount = 0;
    std::int32_t dailyBrandSpotAgentCount = 0;
    double total = 0.0;
};

inline double evaluationTotal(
    const EvaluationBreakdown& result,
    const EvaluationWeights& weights) noexcept {
    return weights.newBrand * result.newBrandCount +
           weights.dailyBrand * result.dailyBrandCount +
           weights.udon * result.udonCount +
           weights.remainingFuel * result.remainingFuel +
           weights.unusedStep * result.unusedSteps +
           weights.valuableSpotDistance * result.valuableSpotDistance +
           weights.newBrandSpotDistance * result.newBrandSpotDistance +
           weights.dailyBrandSpotDistance * result.dailyBrandSpotDistance +
           weights.targetSlack * result.targetSlack +
           weights.supplyDistance * result.supplyDistance +
           weights.rendezvousMismatch * result.rendezvousMismatch +
           weights.fuelRisk * result.fuelRisk +
           weights.unreachableTarget * result.unreachableTargetCount +
           weights.concentration * result.concentrationCount +
           weights.nextDayNewBrand * result.nextDayNewBrandCount +
           weights.nextDayBrand * result.nextDayBrandCount +
           weights.nextDayUdon * result.nextDayUdonCount +
           weights.nextDayNewBrandApproach * result.nextDayNewBrandApproach +
           weights.nextDayBrandApproach * result.nextDayBrandApproach;
}

inline EvaluationBreakdown evaluateGameState(
    const GameState& state,
    const EvaluationContext& context,
    const EvaluationWeights& weights = {}) noexcept {
    EvaluationBreakdown result;
    result.newBrandCount = static_cast<std::int32_t>(countBits(
        state.score.matchBrands & ~context.scoreAtDayStart.matchBrands));
    result.dailyBrandCount = static_cast<std::int32_t>(countBits(state.dayBrands));
    result.udonCount = state.score.totalUdon - context.scoreAtDayStart.totalUdon;
    result.unusedSteps = state.remainingSteps();
    result.concentrationCount = context.concentrationCount;
    result.targetSlack = context.targetSlack;
    result.rendezvousMismatch = context.rendezvousMismatch;
    result.fuelRisk = context.fuelRisk;
    result.unreachableTargetCount = context.unreachableTargetCount;
    result.nextDayNewBrandCount = context.nextDayNewBrandCount;
    result.nextDayBrandCount = context.nextDayBrandCount;
    result.nextDayUdonCount = context.nextDayUdonCount;
    result.nextDayNewBrandApproach = context.nextDayNewBrandApproach;
    result.nextDayBrandApproach = context.nextDayBrandApproach;

    for (std::size_t i = 0; i < state.agentCount; ++i) {
        const Agent& agent = state.agents[i];
        if (agent.isPatrol()) {
            result.remainingFuel += agent.fuel;
            if (context.nearestValuableSpotSteps[i] < kImpassableCost) {
                result.valuableSpotDistance += context.nearestValuableSpotSteps[i];
                ++result.valuableSpotAgentCount;
            }
            if (context.nearestNewBrandSpotSteps[i] < kImpassableCost) {
                result.newBrandSpotDistance +=
                    context.nearestNewBrandSpotSteps[i];
                ++result.newBrandSpotAgentCount;
            }
            if (context.nearestDailyBrandSpotSteps[i] < kImpassableCost) {
                result.dailyBrandSpotDistance +=
                    context.nearestDailyBrandSpotSteps[i];
                ++result.dailyBrandSpotAgentCount;
            }
            if (context.nearestSupplySteps[i] < kImpassableCost) {
                result.supplyDistance += context.nearestSupplySteps[i];
            }
        }
    }

    result.total = evaluationTotal(result, weights);
    return result;
}

}  // namespace benzen
