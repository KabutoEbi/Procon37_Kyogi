#pragma once

#include "game.hpp"
#include "path.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace procon37 {

// 最短ステップ経路を求める基礎戦略。
// 道路状態は1日の間変化しないため、同じ日の探索結果を再利用できる。
class Brain0 final {
public:
    static DijkstraTable dijkstra(
        const MatchSetting& setting,
        const MatchState& state,
        CellPos start) {
        const Field& field = setting.field;
        DijkstraTable table;
        table.start = start;
        table.distances.assign(field.cellCount(), kImpassableCost);
        table.previous.assign(field.cellCount(), kInvalidCell);
        table.previousDirection.assign(
            field.cellCount(), Direction::UpperLeft);

        if (!field.contains(start) || !field.at(start).isPassable()) {
            return table;
        }

        using QueueEntry = std::pair<std::int32_t, CellPos>;
        std::priority_queue<
            QueueEntry,
            std::vector<QueueEntry>,
            std::greater<QueueEntry>>
            queue;

        table.distances[static_cast<std::size_t>(start)] = 0;
        queue.emplace(0, start);

        while (!queue.empty()) {
            const auto [distance, from] = queue.top();
            queue.pop();

            if (distance !=
                table.distances[static_cast<std::size_t>(from)]) {
                continue;
            }

            const std::int32_t edgeCost =
                moveStepCost(setting, state, from);
            if (edgeCost >= kImpassableCost ||
                distance > kImpassableCost - edgeCost) {
                continue;
            }
            const std::int32_t nextDistance = distance + edgeCost;

            for (std::int32_t rawDirection = 0; rawDirection < 6;
                 ++rawDirection) {
                const Direction direction =
                    static_cast<Direction>(rawDirection);
                const CellPos to = field.neighbor(from, direction);
                if (!field.contains(to) || !field.at(to).isPassable()) {
                    continue;
                }

                const std::size_t toIndex = static_cast<std::size_t>(to);
                if (nextDistance >= table.distances[toIndex]) {
                    continue;
                }

                table.distances[toIndex] = nextDistance;
                table.previous[toIndex] = from;
                table.previousDirection[toIndex] = direction;
                queue.emplace(nextDistance, to);
            }
        }
        return table;
    }

    static PathResult restorePath(
        const MatchSetting& setting,
        const DijkstraTable& table,
        CellPos goal,
        AgentKind kind = AgentKind::Patrol) {
        PathResult result;
        result.start = table.start;
        result.goal = goal;

        const Field& field = setting.field;
        if (!field.contains(table.start) || !field.contains(goal) ||
            table.distances.size() != field.cellCount() ||
            table.previous.size() != field.cellCount() ||
            table.previousDirection.size() != field.cellCount() ||
            !table.canReach(goal)) {
            return result;
        }

        result.cost.steps =
            table.distances[static_cast<std::size_t>(goal)];
        result.cost.fuel = 0;

        CellPos current = goal;
        result.positions.push_back(current);
        std::size_t guard = 0;
        while (current != table.start) {
            if (++guard > field.cellCount()) {
                return PathResult{};
            }

            const std::size_t currentIndex =
                static_cast<std::size_t>(current);
            const CellPos previous = table.previous[currentIndex];
            if (!field.contains(previous)) {
                return PathResult{};
            }

            result.directions.push_back(
                table.previousDirection[currentIndex]);
            result.positions.push_back(previous);
            if (kind == AgentKind::Patrol) {
                const std::int32_t fuel =
                    field.at(previous).patrolFuelCost();
                if (fuel >= kImpassableCost ||
                    result.cost.fuel > kImpassableCost - fuel) {
                    return PathResult{};
                }
                result.cost.fuel += fuel;
            }
            current = previous;
        }

        std::reverse(result.positions.begin(), result.positions.end());
        std::reverse(result.directions.begin(), result.directions.end());
        result.reachable = true;
        return result;
    }

    static PathResult shortestPath(
        const MatchSetting& setting,
        const MatchState& state,
        CellPos start,
        CellPos goal,
        AgentKind kind = AgentKind::Patrol) {
        return restorePath(
            setting,
            dijkstra(setting, state, start),
            goal,
            kind);
    }
};

inline DijkstraTable brain0(
    const MatchSetting& setting,
    const MatchState& state,
    CellPos start) {
    return Brain0::dijkstra(setting, state, start);
}

inline PathResult brain0(
    const MatchSetting& setting,
    const MatchState& state,
    CellPos start,
    CellPos goal,
    AgentKind kind = AgentKind::Patrol) {
    return Brain0::shortestPath(setting, state, start, goal, kind);
}

}  // namespace procon37
