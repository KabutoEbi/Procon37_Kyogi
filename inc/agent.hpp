#pragma once

#include "types.hpp"

#include <cstdint>
#include <vector>

namespace procon37 {

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

}  // namespace procon37
