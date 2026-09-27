#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>

namespace benzen {

struct Spot {
    BrandId brand = 0;
    CellPos pos = kInvalidCell;
    std::int32_t maxStock = 0;

    // Field::brands の添字。Field::rebuildSpotIndex() が設定する。
    std::int32_t brandIndex = -1;
};

struct Cell {
    CellType type = CellType::Pond;

    // Field::spots の添字。スポットがなければ -1。
    std::int32_t spotIndex = -1;

    constexpr bool isPassable() const noexcept {
        return type != CellType::Pond;
    }

    constexpr bool hasSpot() const noexcept {
        return spotIndex >= 0;
    }

    // このセルから隣へ移動するためのステップ数。
    // 池の場合は kImpassableCost を返す。
    constexpr std::int32_t moveSteps(
        TrafficStatus traffic = TrafficStatus::Smooth) const noexcept {
        switch (type) {
            case CellType::Plain:
                return 2;
            case CellType::Road:
                switch (traffic) {
                    case TrafficStatus::Smooth:
                        return 1;
                    case TrafficStatus::Busy:
                        return 2;
                    case TrafficStatus::Jammed:
                        return 4;
                }
                return kImpassableCost;
            case CellType::Mountain:
                return 3;
            case CellType::Pond:
                return kImpassableCost;
        }
        return kImpassableCost;
    }

    // 巡回車がこのセルから移動するときの燃料消費。
    // 補給車の消費は地形によらず 0。
    constexpr std::int32_t patrolFuelCost() const noexcept {
        switch (type) {
            case CellType::Plain:
                return 1;
            case CellType::Road:
            case CellType::Mountain:
                return 2;
            case CellType::Pond:
                return kImpassableCost;
        }
        return kImpassableCost;
    }
};

struct TrafficInfo {
    CellPos pos = kInvalidCell;
    TrafficStatus status = TrafficStatus::Smooth;
};

}  // namespace benzen
