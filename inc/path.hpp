#pragma once

#include "agent.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace benzen {

struct RouteCost {
    std::int32_t steps = kImpassableCost;
    std::int32_t fuel = kImpassableCost;

    constexpr bool reachable() const noexcept { return steps < kImpassableCost; }
};

struct PathResult {
    bool reachable = false;
    CellPos start = kInvalidCell;
    CellPos goal = kInvalidCell;
    RouteCost cost{};
    std::vector<CellPos> positions;
    std::vector<Direction> directions;
};

// 指定stepの補給フェーズで、補給車と同じセルにいると満タンになる予定。
struct TimedRefuel {
    CellPos pos = kInvalidCell;
    std::int32_t step = 0;
};

// 移動に加えて補給待ちを含む経路。actionsはそのままAgentPlanとして使える。
struct TimedPathResult {
    bool reachable = false;
    CellPos start = kInvalidCell;
    CellPos goal = kInvalidCell;
    std::int32_t startStep = 0;
    std::int32_t arrivalStep = kImpassableCost;
    std::int32_t finalFuel = 0;
    std::int32_t refuelCount = 0;
    RouteCost cost{};
    std::vector<CellPos> positions;
    std::vector<Direction> directions;
    AgentPlan actions;
};

// 全セルへのダイクストラ結果。goalごとの経路を前駆配列から復元する。
struct DijkstraTable {
    CellPos start = kInvalidCell;
    std::vector<std::int32_t> distances;
    // Patrol fuel along this exact shortest-time predecessor path (not a
    // minimum-fuel distance). Allows allocation-free placement evaluation.
    std::vector<std::int32_t> patrolFuelCosts;
    std::vector<CellPos> previous;
    std::vector<Direction> previousDirection;
    std::vector<CellPos> firstHop;

    bool canReach(CellPos pos) const noexcept {
        return pos >= 0 && static_cast<std::size_t>(pos) < distances.size() &&
               distances[static_cast<std::size_t>(pos)] < kImpassableCost;
    }
};

// 燃料を頂点状態へ含めるダイクストラで使用する。
struct FuelPathState {
    CellPos pos = kInvalidCell;
    std::int32_t fuel = 0;

    friend constexpr bool operator==(FuelPathState lhs, FuelPathState rhs) noexcept {
        return lhs.pos == rhs.pos && lhs.fuel == rhs.fuel;
    }
};

struct FuelPathLabel {
    FuelPathState state{};
    std::int32_t steps = kImpassableCost;
    std::int32_t previousLabel = -1;
    Direction previousDirection = Direction::UpperLeft;
};

} // namespace benzen
