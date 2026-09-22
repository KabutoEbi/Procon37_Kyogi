#pragma once

#include "../dial_queue.hpp"
#include "../game.hpp"
#include "../path.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
        CellPos start,
        const std::function<bool()>& stop = {}) {
        const Field& f = s.field;
        DijkstraTable res;
        res.start = start;
        res.distances.assign(f.cellCount(), kImpassableCost);
        res.previous.assign(f.cellCount(), kInvalidCell);
        res.previousDirection.assign(f.cellCount(), Direction::UpperLeft);
        res.firstHop.assign(f.cellCount(), kInvalidCell);

        if (!f.contains(start) || !f.at(start).isPassable()) {
            return res;
        }

        DialQueue<CellPos> q;

        res.distances[static_cast<std::size_t>(start)] = 0;
        q.push(0, start);

        while (!q.empty()) {
            if (stop && stop()) return res;
            const auto [dist, from] = q.pop();

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
                res.firstHop[i] = from == start ? to : res.firstHop[from];
                q.push(next, to);
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

    // 燃料消費量を状態に含め、指定燃料内で所要ステップが最小の経路を求める。
    // 補給は扱わない。補給車は燃料を消費しないため通常の最短路と一致する。
    static PathResult shortestFuelConstrainedPath(
        const MatchSetting& s,
        const MatchState& st,
        CellPos start,
        CellPos goal,
        AgentKind kind,
        std::int32_t fuelBudget,
        const std::function<bool()>& stop = {}) {
        PathResult res;
        res.start = start;
        res.goal = goal;
        const Field& f = s.field;
        if (fuelBudget < 0 || !f.contains(start) || !f.contains(goal) ||
            !f.at(start).isPassable() || !f.at(goal).isPassable()) {
            return res;
        }
        if (kind == AgentKind::Supply) {
            return shortestPath(s, st, start, goal, kind);
        }

        const std::int32_t usefulFuel = static_cast<std::int32_t>(
            std::min<std::int64_t>(
                fuelBudget,
                2LL * static_cast<std::int64_t>(f.cellCount())));
        const std::size_t stride = static_cast<std::size_t>(usefulFuel) + 1;
        if (stride == 0 || f.cellCount() >
                std::numeric_limits<std::size_t>::max() / stride) {
            return res;
        }
        const std::size_t stateCount = f.cellCount() * stride;
        std::vector<std::int32_t> distances(stateCount, kImpassableCost);
        std::vector<std::int32_t> previous(stateCount, -1);
        std::vector<Direction> directions(
            stateCount, Direction::UpperLeft);
        auto index = [stride](CellPos pos, std::int32_t used) {
            return static_cast<std::size_t>(pos) * stride +
                   static_cast<std::size_t>(used);
        };

        DialQueue<std::pair<CellPos, std::int32_t>> queue;
        distances[index(start, 0)] = 0;
        queue.push(0, {start, 0});
        while (!queue.empty()) {
            if (stop && stop()) {
                return PathResult{};
            }
            const auto [steps, state] = queue.pop();
            const CellPos from = state.first;
            const std::int32_t used = state.second;
            const std::size_t fromIndex = index(from, used);
            if (steps != distances[fromIndex]) {
                continue;
            }
            const std::int32_t edgeSteps = moveStepCost(s, st, from);
            const std::int32_t edgeFuel =
                moveFuelCost(s, AgentKind::Patrol, from);
            if (edgeSteps >= kImpassableCost || edgeFuel < 0 ||
                used > usefulFuel - edgeFuel ||
                steps > kImpassableCost - edgeSteps) {
                continue;
            }
            const std::int32_t nextSteps = steps + edgeSteps;
            const std::int32_t nextFuel = used + edgeFuel;
            for (std::int32_t d = 0; d < 6; ++d) {
                const auto direction = static_cast<Direction>(d);
                const CellPos to = f.neighbor(from, direction);
                if (!f.contains(to) || !f.at(to).isPassable()) {
                    continue;
                }
                const std::size_t toIndex = index(to, nextFuel);
                if (nextSteps >= distances[toIndex]) {
                    continue;
                }
                distances[toIndex] = nextSteps;
                previous[toIndex] = static_cast<std::int32_t>(fromIndex);
                directions[toIndex] = direction;
                queue.push(nextSteps, {to, nextFuel});
            }
        }

        std::size_t best = stateCount;
        for (std::int32_t fuel = 0; fuel <= usefulFuel; ++fuel) {
            const std::size_t candidate = index(goal, fuel);
            if (best == stateCount ||
                distances[candidate] < distances[best] ||
                (distances[candidate] == distances[best] &&
                 fuel < static_cast<std::int32_t>(best % stride))) {
                best = candidate;
            }
        }
        if (best == stateCount || distances[best] >= kImpassableCost) {
            return res;
        }

        res.cost.steps = distances[best];
        res.cost.fuel = static_cast<std::int32_t>(best % stride);
        std::size_t current = best;
        res.positions.push_back(goal);
        while (current != index(start, 0)) {
            const std::int32_t before = previous[current];
            if (before < 0 || static_cast<std::size_t>(before) >= stateCount) {
                return PathResult{};
            }
            res.directions.push_back(directions[current]);
            current = static_cast<std::size_t>(before);
            res.positions.push_back(static_cast<CellPos>(current / stride));
        }
        std::reverse(res.positions.begin(), res.positions.end());
        std::reverse(res.directions.begin(), res.directions.end());
        res.reachable = true;
        return res;
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

inline PathResult brain0FuelConstrained(
    const MatchSetting& setting,
    const MatchState& state,
    CellPos start,
    CellPos goal,
    AgentKind kind,
    std::int32_t fuelBudget) {
    return Brain0::shortestFuelConstrainedPath(
        setting, state, start, goal, kind, fuelBudget);
}

}  // namespace procon37
