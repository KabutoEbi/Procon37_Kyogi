#pragma once

#include "types.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace benzen {

struct Agent {
    // setting.agents および state.agents 内での不変な添字。
    AgentIndex index = 0;
    AgentKind kind = AgentKind::Patrol;
    CellPos pos = kInvalidCell;
    std::int32_t fuel = 0;

    constexpr bool isPatrol() const noexcept {
        return kind == AgentKind::Patrol;
    }

    constexpr bool isSupply() const noexcept {
        return kind == AgentKind::Supply;
    }
};

struct TeamState {
    PlayerId id = 0;
    std::vector<Agent> agents;
};

// APIへ送る値をそのまま保持する。0～5が移動、負数が待機。
using AgentPlan = std::vector<std::int32_t>;
using DailyPlan = std::vector<AgentPlan>;

// 連続する待機をAPI形式の1要素へまとめる。
// 例: -1, -1, -3, 2, -1 -> -5, 2, -1。
inline void appendPlanAction(AgentPlan& plan, std::int32_t action) {
    if (!isWaitAction(action) || plan.empty() ||
        !isWaitAction(plan.back())) {
        plan.push_back(action);
        return;
    }

    const std::int64_t combined =
        static_cast<std::int64_t>(plan.back()) + action;
    if (combined >= std::numeric_limits<std::int32_t>::min()) {
        plan.back() = static_cast<std::int32_t>(combined);
    } else {
        plan.push_back(action);
    }
}

inline AgentPlan compressWaitActions(const AgentPlan& plan) {
    AgentPlan result;
    result.reserve(plan.size());
    for (const std::int32_t action : plan) {
        appendPlanAction(result, action);
    }
    return result;
}

inline DailyPlan compressWaitActions(const DailyPlan& plan) {
    DailyPlan result;
    result.reserve(plan.size());
    for (const AgentPlan& actions : plan) {
        result.push_back(compressWaitActions(actions));
    }
    return result;
}

}  // namespace benzen
