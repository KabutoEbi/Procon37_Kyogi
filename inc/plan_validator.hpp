#pragma once

#include "simulator.hpp"

namespace benzen {

struct PlanValidationResult {
    bool valid = false;
    SimulationFailure failure{};
};

// ステップ数、移動可能性、燃料、補給タイミングを含めて検査する。
// 判定規則を二重実装しないため、公式順序のシミュレータを利用する。
inline PlanValidationResult validateDailyPlan(
    const MatchSetting& setting,
    const MatchState& matchState,
    const DailyPlan& plan,
    ScoreState previousScore = {}) {
    const SimulationResult result =
        simulateDay(setting, matchState, plan, previousScore);
    return {result.valid, result.failure};
}

}  // namespace benzen
