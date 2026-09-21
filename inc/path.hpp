#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace procon37 {

struct RouteCost {
    std::int32_t steps = kImpassableCost;
    std::int32_t fuel = kImpassableCost;

    constexpr bool reachable() const noexcept {
        return steps < kImpassableCost;
    }
};

struct PathResult {
    bool reachable = false;
    CellPos start = kInvalidCell;
    CellPos goal = kInvalidCell;
    RouteCost cost{};
    std::vector<CellPos> positions;
    std::vector<Direction> directions;
};

// 全セルへのダイクストラ結果。goalごとの経路を前駆配列から復元する。
struct DijkstraTable {
    CellPos start = kInvalidCell;
    std::vector<std::int32_t> distances;
    std::vector<CellPos> previous;
    std::vector<Direction> previousDirection;
    std::vector<CellPos> firstHop;

    bool canReach(CellPos pos) const noexcept {
        return pos >= 0 &&
               static_cast<std::size_t>(pos) < distances.size() &&
               distances[static_cast<std::size_t>(pos)] < kImpassableCost;
    }
};

// 燃料を頂点状態へ含めるダイクストラで使用する。
struct FuelPathState {
    CellPos pos = kInvalidCell;
    std::int32_t fuel = 0;

    friend constexpr bool operator==(
        FuelPathState lhs,
        FuelPathState rhs) noexcept {
        return lhs.pos == rhs.pos && lhs.fuel == rhs.fuel;
    }
};

struct FuelPathLabel {
    FuelPathState state{};
    std::int32_t steps = kImpassableCost;
    std::int32_t previousLabel = -1;
    Direction previousDirection = Direction::UpperLeft;
};

}  // namespace procon37
