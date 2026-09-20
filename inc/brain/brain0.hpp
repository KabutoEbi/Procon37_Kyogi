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
        const MatchSetting& s,
        const MatchState& st,
        CellPos start) {
        const Field& f = s.field;
        DijkstraTable res;
        res.start = start;
        res.distances.assign(f.cellCount(), kImpassableCost);
        res.previous.assign(f.cellCount(), kInvalidCell);
        res.previousDirection.assign(f.cellCount(), Direction::UpperLeft);

        if (!f.contains(start) || !f.at(start).isPassable()) {
            return res;
        }

        using QueueEntry = std::pair<std::int32_t, CellPos>;
        std::priority_queue<
            QueueEntry,
            std::vector<QueueEntry>,
            std::greater<QueueEntry>>
            q;

        res.distances[static_cast<std::size_t>(start)] = 0;
        q.emplace(0, start);

        while (!q.empty()) {
            const auto [dist, from] = q.top();
            q.pop();

            if (dist != res.distances[static_cast<std::size_t>(from)]) {
                continue;
            }

            const std::int32_t cost = moveStepCost(s, st, from);
            if (cost >= kImpassableCost || dist > kImpassableCost - cost) {
                continue;
            }
            const std::int32_t next = dist + cost;

            for (std::int32_t d = 0; d < 6; ++d) {
                const auto dir = static_cast<Direction>(d);
                const CellPos to = f.neighbor(from, dir);
                if (!f.contains(to) || !f.at(to).isPassable()) {
                    continue;
                }

                const auto i = static_cast<std::size_t>(to);
                if (next >= res.distances[i]) {
                    continue;
                }

                res.distances[i] = next;
                res.previous[i] = from;
                res.previousDirection[i] = dir;
                q.emplace(next, to);
            }
        }
        return res;
    }

    static PathResult restorePath(
        const MatchSetting& s,
        const DijkstraTable& t,
        CellPos goal,
        AgentKind kind = AgentKind::Patrol) {
        PathResult res;
        res.start = t.start;
        res.goal = goal;

        const Field& f = s.field;
        if (!f.contains(t.start) || !f.contains(goal) ||
            t.distances.size() != f.cellCount() ||
            t.previous.size() != f.cellCount() ||
            t.previousDirection.size() != f.cellCount() ||
            !t.canReach(goal)) {
            return res;
        }

        res.cost.steps = t.distances[static_cast<std::size_t>(goal)];
        res.cost.fuel = 0;

        CellPos cur = goal;
        res.positions.push_back(cur);
        std::size_t guard = 0;
        while (cur != t.start) {
            if (++guard > f.cellCount()) {
                return PathResult{};
            }

            const auto i = static_cast<std::size_t>(cur);
            const CellPos prev = t.previous[i];
            if (!f.contains(prev)) {
                return PathResult{};
            }

            res.directions.push_back(t.previousDirection[i]);
            res.positions.push_back(prev);
            if (kind == AgentKind::Patrol) {
                const std::int32_t fuel = f.at(prev).patrolFuelCost();
                if (fuel >= kImpassableCost ||
                    res.cost.fuel > kImpassableCost - fuel) {
                    return PathResult{};
                }
                res.cost.fuel += fuel;
            }
            cur = prev;
        }

        std::reverse(res.positions.begin(), res.positions.end());
        std::reverse(res.directions.begin(), res.directions.end());
        res.reachable = true;
        return res;
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
