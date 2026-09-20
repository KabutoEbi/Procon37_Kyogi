#pragma once

#include "cell.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace procon37 {

struct Field {
    std::int32_t width = 0;
    std::int32_t height = 0;

    // pos = y * width + x の行優先配列。
    std::vector<Cell> cells;
    std::vector<Spot> spots;

    // spots[].brand を重複なしで並べたもの。brandIndex から元IDを引ける。
    std::vector<BrandId> brands;

    constexpr std::size_t cellCount() const noexcept {
        return static_cast<std::size_t>(width) *
               static_cast<std::size_t>(height);
    }

    constexpr bool contains(Coord coord) const noexcept {
        return 0 <= coord.x && coord.x < width &&
               0 <= coord.y && coord.y < height;
    }

    constexpr bool contains(CellPos pos) const noexcept {
        return 0 <= pos &&
               static_cast<std::size_t>(pos) < cellCount();
    }

    constexpr CellPos toPos(Coord coord) const noexcept {
        return contains(coord) ? coord.y * width + coord.x : kInvalidCell;
    }

    constexpr Coord toCoord(CellPos pos) const noexcept {
        return contains(pos) ? Coord{pos % width, pos / width}
                             : Coord{-1, -1};
    }

    const Cell& at(CellPos pos) const noexcept {
        assert(contains(pos));
        return cells[static_cast<std::size_t>(pos)];
    }

    Cell& at(CellPos pos) noexcept {
        assert(contains(pos));
        return cells[static_cast<std::size_t>(pos)];
    }

    const Spot* spotAt(CellPos pos) const noexcept {
        if (!contains(pos)) {
            return nullptr;
        }
        const std::int32_t index = at(pos).spotIndex;
        if (index < 0 || static_cast<std::size_t>(index) >= spots.size()) {
            return nullptr;
        }
        return &spots[static_cast<std::size_t>(index)];
    }

    // 指定方向の隣接セル。盤外なら kInvalidCell。
    constexpr CellPos neighbor(CellPos from, Direction direction) const noexcept {
        if (!contains(from)) {
            return kInvalidCell;
        }

        const Coord origin = toCoord(from);
        Coord next = origin;
        const bool evenRow = (origin.y % 2) == 0;

        switch (direction) {
            case Direction::UpperLeft:
                next.x += evenRow ? 0 : -1;
                --next.y;
                break;
            case Direction::UpperRight:
                next.x += evenRow ? 1 : 0;
                --next.y;
                break;
            case Direction::Right:
                ++next.x;
                break;
            case Direction::LowerRight:
                next.x += evenRow ? 1 : 0;
                ++next.y;
                break;
            case Direction::LowerLeft:
                next.x += evenRow ? 0 : -1;
                ++next.y;
                break;
            case Direction::Left:
                --next.x;
                break;
        }
        return toPos(next);
    }

    // 方向値の順（左上から時計回り）で全隣接セルを返す。
    constexpr std::array<CellPos, 6> neighbors(CellPos from) const noexcept {
        return {
            neighbor(from, Direction::UpperLeft),
            neighbor(from, Direction::UpperRight),
            neighbor(from, Direction::Right),
            neighbor(from, Direction::LowerRight),
            neighbor(from, Direction::LowerLeft),
            neighbor(from, Direction::Left),
        };
    }

    // cells と spots の対応を構築する。JSON読込後に一度呼ぶ。
    // false の場合、セル数・位置・地形などに不整合がある。
    bool rebuildSpotIndex() {
        if (width <= 0 || height <= 0 || cells.size() != cellCount() ||
            spots.size() > kMaxSpots) {
            return false;
        }

        std::vector<std::int32_t> spotIds(cellCount(), -1);
        std::vector<std::int32_t> brandIds(spots.size(), -1);
        std::vector<BrandId> nextBrands;
        for (std::size_t i = 0; i < spots.size(); ++i) {
            const Spot& s = spots[i];
            if (!contains(s.pos) || s.maxStock <= 0) {
                return false;
            }
            const auto pos = static_cast<std::size_t>(s.pos);
            if (cells[pos].type != CellType::Plain || spotIds[pos] >= 0) {
                return false;
            }
            spotIds[pos] = static_cast<std::int32_t>(i);

            auto it = std::find(nextBrands.begin(), nextBrands.end(), s.brand);
            if (it == nextBrands.end()) {
                if (nextBrands.size() >= kMaxBrands) {
                    return false;
                }
                nextBrands.push_back(s.brand);
                it = std::prev(nextBrands.end());
            }
            brandIds[i] = static_cast<std::int32_t>(
                it - nextBrands.begin());
        }
        for (std::size_t pos = 0; pos < cells.size(); ++pos) {
            cells[pos].spotIndex = spotIds[pos];
        }
        for (std::size_t i = 0; i < spots.size(); ++i) {
            spots[i].brandIndex = brandIds[i];
        }
        brands = std::move(nextBrands);
        return true;
    }
};

}  // namespace procon37
