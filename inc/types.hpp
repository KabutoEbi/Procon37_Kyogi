#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace benzen {

using CellPos = std::int32_t;
using BrandId = std::int32_t;
using PlayerId = std::int32_t;
using AgentIndex = std::size_t;

inline constexpr CellPos kInvalidCell = -1;
inline constexpr std::size_t kMaxAgents = 8;
inline constexpr std::size_t kMaxSpots = 32;
inline constexpr std::size_t kMaxBrands = 32;
inline constexpr std::int32_t kImpassableCost =
    std::numeric_limits<std::int32_t>::max() / 4;

using SpotMask = std::uint64_t;
using BrandMask = std::uint64_t;

// JSON cells[y][x] の値と一致する。
enum class CellType : std::uint8_t {
    Plain = 0,
    Road = 1,
    Mountain = 2,
    Pond = 3,
};

// API の traffics[].status と一致する。
enum class TrafficStatus : std::uint8_t {
    Smooth = 0,
    Busy = 1,
    Jammed = 2,
};

// API の agents[].kind と一致する。
enum class AgentKind : std::uint8_t {
    Patrol = 0,
    Supply = 1,
};

// 行動値 0～5 と一致する。偶数行が右へずれる座標系を用いる。
enum class Direction : std::uint8_t {
    UpperLeft = 0,
    UpperRight = 1,
    Right = 2,
    LowerRight = 3,
    LowerLeft = 4,
    Left = 5,
};

struct Coord {
    std::int32_t x = 0;
    std::int32_t y = 0;

    friend constexpr bool operator==(Coord lhs, Coord rhs) noexcept {
        return lhs.x == rhs.x && lhs.y == rhs.y;
    }

    friend constexpr bool operator!=(Coord lhs, Coord rhs) noexcept {
        return !(lhs == rhs);
    }
};

constexpr std::int32_t toAction(Direction direction) noexcept {
    return static_cast<std::int32_t>(direction);
}

constexpr bool isMoveAction(std::int32_t action) noexcept {
    return 0 <= action && action <= 5;
}

constexpr bool isWaitAction(std::int32_t action) noexcept {
    return action <= -1;
}

constexpr std::int32_t waitSteps(std::int32_t action) noexcept {
    return isWaitAction(action) ? -action : 0;
}

}  // namespace benzen
