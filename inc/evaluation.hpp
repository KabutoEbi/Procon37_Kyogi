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
    const EvaluationBreakdown& res,
    const EvaluationWeights& w) noexcept {
    return w.newBrand * res.newBrandCount +
           w.dailyBrand * res.dailyBrandCount +
           w.udon * res.udonCount +
           w.remainingFuel * res.remainingFuel +
           w.unusedStep * res.unusedSteps +
           w.valuableSpotDistance * res.valuableSpotDistance +
           w.newBrandSpotDistance * res.newBrandSpotDistance +
           w.dailyBrandSpotDistance * res.dailyBrandSpotDistance +
           w.targetSlack * res.targetSlack +
           w.supplyDistance * res.supplyDistance +
           w.rendezvousMismatch * res.rendezvousMismatch +
           w.fuelRisk * res.fuelRisk +
           w.unreachableTarget * res.unreachableTargetCount +
           w.concentration * res.concentrationCount +
           w.nextDayNewBrand * res.nextDayNewBrandCount +
           w.nextDayBrand * res.nextDayBrandCount +
           w.nextDayUdon * res.nextDayUdonCount +
           w.nextDayNewBrandApproach * res.nextDayNewBrandApproach +
           w.nextDayBrandApproach * res.nextDayBrandApproach;
}

inline EvaluationBreakdown evaluateGameState(
    const GameState& st,
    const EvaluationContext& ctx,
    const EvaluationWeights& w = {}) noexcept {
    EvaluationBreakdown res;
    res.newBrandCount = static_cast<std::int32_t>(countBits(
        st.score.matchBrands & ~ctx.scoreAtDayStart.matchBrands));
    res.dailyBrandCount = static_cast<std::int32_t>(countBits(st.dayBrands));
    res.udonCount = st.score.totalUdon - ctx.scoreAtDayStart.totalUdon;
    res.unusedSteps = st.remainingSteps();
    res.concentrationCount = ctx.concentrationCount;
    res.targetSlack = ctx.targetSlack;
    res.rendezvousMismatch = ctx.rendezvousMismatch;
    res.fuelRisk = ctx.fuelRisk;
    res.unreachableTargetCount = ctx.unreachableTargetCount;
    res.nextDayNewBrandCount = ctx.nextDayNewBrandCount;
    res.nextDayBrandCount = ctx.nextDayBrandCount;
    res.nextDayUdonCount = ctx.nextDayUdonCount;
    res.nextDayNewBrandApproach = ctx.nextDayNewBrandApproach;
    res.nextDayBrandApproach = ctx.nextDayBrandApproach;

    for (std::size_t i = 0; i < st.agentCount; ++i) {
        const Agent& a = st.agents[i];
        if (a.isPatrol()) {
            res.remainingFuel += a.fuel;
            if (ctx.nearestValuableSpotSteps[i] < kImpassableCost) {
                res.valuableSpotDistance += ctx.nearestValuableSpotSteps[i];
                ++res.valuableSpotAgentCount;
            }
            if (ctx.nearestNewBrandSpotSteps[i] < kImpassableCost) {
                res.newBrandSpotDistance +=
                    ctx.nearestNewBrandSpotSteps[i];
                ++res.newBrandSpotAgentCount;
            }
            if (ctx.nearestDailyBrandSpotSteps[i] < kImpassableCost) {
                res.dailyBrandSpotDistance +=
                    ctx.nearestDailyBrandSpotSteps[i];
                ++res.dailyBrandSpotAgentCount;
            }
            if (ctx.nearestSupplySteps[i] < kImpassableCost) {
                res.supplyDistance += ctx.nearestSupplySteps[i];
            }
        }
    }

    res.total = evaluationTotal(res, w);
    return res;
}

}  // namespace procon37
