#pragma once

#include "../dial_queue.hpp"
#include "../game_state.hpp"
#include "../path.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace benzen {

// 最短ステップ経路を求める基礎戦略。
// 道路状態は1日の間変化しないため、同じ日の探索結果を再利用できる。
class Brain0 final {
  public:
    static DijkstraTable dijkstra(const MatchSetting& s, const MatchState& st, CellPos start,
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
            if (stop && stop())
                return res;
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

    static PathResult restorePath(const MatchSetting& s, const DijkstraTable& t, CellPos goal,
                                  AgentKind kind = AgentKind::Patrol) {
        PathResult res;
        res.start = t.start;
        res.goal = goal;

        const Field& f = s.field;
        if (!f.contains(t.start) || !f.contains(goal) || t.distances.size() != f.cellCount() ||
            t.previous.size() != f.cellCount() || t.previousDirection.size() != f.cellCount() ||
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
                if (fuel >= kImpassableCost || res.cost.fuel > kImpassableCost - fuel) {
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

    static PathResult shortestPath(const MatchSetting& setting, const MatchState& state,
                                   CellPos start, CellPos goal,
                                   AgentKind kind = AgentKind::Patrol) {
        return restorePath(setting, dijkstra(setting, state, start), goal, kind);
    }

    // 燃料消費量を状態に含め、指定燃料内で所要ステップが最小の経路を求める。
    // 補給は扱わない。補給車は燃料を消費しないため通常の最短路と一致する。
    static PathResult shortestFuelConstrainedPath(const MatchSetting& s, const MatchState& st,
                                                  CellPos start, CellPos goal, AgentKind kind,
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
            std::min<std::int64_t>(fuelBudget, 2LL * static_cast<std::int64_t>(f.cellCount())));
        const std::size_t stride = static_cast<std::size_t>(usefulFuel) + 1;
        if (stride == 0 || f.cellCount() > std::numeric_limits<std::size_t>::max() / stride) {
            return res;
        }
        const std::size_t stateCount = f.cellCount() * stride;
        std::vector<std::int32_t> distances(stateCount, kImpassableCost);
        std::vector<std::int32_t> previous(stateCount, -1);
        std::vector<Direction> directions(stateCount, Direction::UpperLeft);
        auto index = [stride](CellPos pos, std::int32_t used) {
            return static_cast<std::size_t>(pos) * stride + static_cast<std::size_t>(used);
        };

        DialQueue<std::pair<CellPos, std::int32_t>> queue;
        distances[index(start, 0)] = 0;
        queue.push(0, {start, 0});
        std::int32_t goalSteps = kImpassableCost;
        while (!queue.empty()) {
            if (stop && stop()) {
                return PathResult{};
            }
            const auto [steps, state] = queue.pop();
            // 正の移動コストなので、最初の目的地より遅い経路は不要。
            // 同時刻の目的地ラベルは残し、最小燃料のタイブレークを保つ。
            if (steps > goalSteps)
                break;
            const CellPos from = state.first;
            const std::int32_t used = state.second;
            const std::size_t fromIndex = index(from, used);
            if (steps != distances[fromIndex]) {
                continue;
            }
            if (from == goal) {
                goalSteps = steps;
                continue;
            }
            const std::int32_t edgeSteps = moveStepCost(s, st, from);
            const std::int32_t edgeFuel = moveFuelCost(s, AgentKind::Patrol, from);
            if (edgeSteps >= kImpassableCost || edgeFuel < 0 || used > usefulFuel - edgeFuel ||
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
            if (best == stateCount || distances[candidate] < distances[best] ||
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

    // 補給車が指定セルにいるstepを経由候補にし、到着までの最短時間を求める。
    // 各補給予定では、早着時の待機後に公式フェーズ順で満タンになる。
    static TimedPathResult
    shortestPathWithTimedRefuels(const MatchSetting& s, const MatchState& st, CellPos start,
                                 CellPos goal, AgentKind kind, std::int32_t initialFuel,
                                 std::int32_t fuelLimit, std::int32_t startStep,
                                 std::int32_t stepLimit, const std::vector<TimedRefuel>& refuels,
                                 const std::function<bool()>& stop = {}) {
        TimedPathResult result;
        result.start = start;
        result.goal = goal;
        result.startStep = startStep;
        const Field& field = s.field;
        if (initialFuel < 0 || fuelLimit < 0 || initialFuel > fuelLimit || startStep < 0 ||
            stepLimit < startStep || !field.contains(start) || !field.contains(goal) ||
            !field.at(start).isPassable() || !field.at(goal).isPassable()) {
            return result;
        }
        if (kind == AgentKind::Supply) {
            const auto path = shortestPath(s, st, start, goal, kind);
            if (!path.reachable || path.cost.steps > stepLimit - startStep)
                return result;
            result.reachable = true;
            result.arrivalStep = startStep + path.cost.steps;
            result.finalFuel = initialFuel;
            result.cost = path.cost;
            result.positions = path.positions;
            result.directions = path.directions;
            for (const Direction direction : path.directions)
                result.actions.push_back(toAction(direction));
            return result;
        }

        const std::size_t cells = field.cellCount();
        const std::size_t times = static_cast<std::size_t>(stepLimit - startStep + 1);
        if (cells == 0 || times > std::numeric_limits<std::size_t>::max() / cells) {
            return result;
        }
        std::vector<bool> refuelAt(times * cells, false);
        auto stateIndex = [&](std::int32_t step, CellPos pos) {
            return static_cast<std::size_t>(step - startStep) * cells +
                   static_cast<std::size_t>(pos);
        };
        for (const TimedRefuel& event : refuels) {
            if (event.step <= startStep || event.step > stepLimit || !field.contains(event.pos) ||
                !field.at(event.pos).isPassable())
                continue;
            refuelAt[stateIndex(event.step, event.pos)] = true;
        }

        struct Label {
            std::int32_t fuel = -1;
            std::int32_t refuelCount = kImpassableCost;
            std::int32_t consumedFuel = kImpassableCost;
            std::int32_t previousStep = -1;
            CellPos previousPos = kInvalidCell;
            std::int32_t action = 0;
        };
        std::vector<Label> labels(times * cells);
        labels[stateIndex(startStep, start)] = {initialFuel, 0, 0, -1, kInvalidCell, 0};

        auto applyRefuel = [&](std::int32_t step, CellPos pos, std::int32_t& fuel,
                               std::int32_t& count) {
            if (refuelAt[stateIndex(step, pos)] && fuel < fuelLimit) {
                fuel = fuelLimit;
                ++count;
            }
        };
        auto improve = [&](std::int32_t step, CellPos pos, std::int32_t fuel, std::int32_t count,
                           std::int32_t consumed, std::int32_t previousStep, CellPos previousPos,
                           std::int32_t action) {
            Label& label = labels[stateIndex(step, pos)];
            if (fuel < label.fuel ||
                (fuel == label.fuel &&
                 std::tie(count, consumed) >= std::tie(label.refuelCount, label.consumedFuel))) {
                return;
            }
            label = {fuel, count, consumed, previousStep, previousPos, action};
        };

        std::int32_t arrival = kImpassableCost;
        for (std::int32_t step = startStep; step <= stepLimit; ++step) {
            if (stop && stop())
                return TimedPathResult{};
            if (labels[stateIndex(step, goal)].fuel >= 0) {
                arrival = step;
                break;
            }
            if (step == stepLimit)
                break;
            for (CellPos from = 0; from < static_cast<CellPos>(cells); ++from) {
                const Label current = labels[stateIndex(step, from)];
                if (current.fuel < 0)
                    continue;

                std::int32_t waitFuel = current.fuel;
                std::int32_t waitRefuels = current.refuelCount;
                applyRefuel(step + 1, from, waitFuel, waitRefuels);
                improve(step + 1, from, waitFuel, waitRefuels, current.consumedFuel, step, from,
                        -1);

                const std::int32_t edgeSteps = moveStepCost(s, st, from);
                const std::int32_t edgeFuel = moveFuelCost(s, AgentKind::Patrol, from);
                if (edgeSteps >= kImpassableCost || edgeFuel < 0 || edgeSteps > stepLimit - step)
                    continue;
                for (std::int32_t d = 0; d < 6; ++d) {
                    const auto direction = static_cast<Direction>(d);
                    const CellPos to = field.neighbor(from, direction);
                    if (!field.contains(to) || !field.at(to).isPassable())
                        continue;
                    std::int32_t nextFuel = current.fuel;
                    std::int32_t nextRefuels = current.refuelCount;
                    for (std::int32_t during = step + 1; during < step + edgeSteps; ++during) {
                        applyRefuel(during, from, nextFuel, nextRefuels);
                    }
                    if (nextFuel < edgeFuel)
                        continue;
                    nextFuel -= edgeFuel;
                    applyRefuel(step + edgeSteps, to, nextFuel, nextRefuels);
                    improve(step + edgeSteps, to, nextFuel, nextRefuels,
                            current.consumedFuel + edgeFuel, step, from, toAction(direction));
                }
            }
        }
        if (arrival >= kImpassableCost)
            return result;

        CellPos currentPos = goal;
        std::int32_t currentStep = arrival;
        AgentPlan reversedActions;
        while (currentStep != startStep || currentPos != start) {
            const Label& label = labels[stateIndex(currentStep, currentPos)];
            if (label.previousStep < startStep || !field.contains(label.previousPos))
                return TimedPathResult{};
            reversedActions.push_back(label.action);
            currentStep = label.previousStep;
            currentPos = label.previousPos;
        }
        std::reverse(reversedActions.begin(), reversedActions.end());
        result.positions.push_back(start);
        CellPos pos = start;
        for (const std::int32_t action : reversedActions) {
            if (isWaitAction(action)) {
                appendPlanAction(result.actions, action);
                continue;
            }
            const auto direction = static_cast<Direction>(action);
            result.actions.push_back(action);
            result.directions.push_back(direction);
            pos = field.neighbor(pos, direction);
            result.positions.push_back(pos);
        }
        const Label& goalLabel = labels[stateIndex(arrival, goal)];
        result.reachable = true;
        result.arrivalStep = arrival;
        result.finalFuel = goalLabel.fuel;
        result.refuelCount = goalLabel.refuelCount;
        result.cost.steps = arrival - startStep;
        result.cost.fuel = goalLabel.consumedFuel;
        return result;
    }

    // 各巡回車が未取得系列を優先して近いスポットへ進む比較用ベースライン。
    // 補給車は待機し、巡回車は現在燃料だけで完了できる経路を選ぶ。
    static DailyPlan greedyPlan(const MatchSetting& s, const MatchState& st,
                                ScoreState previous = {}) {
        if (!s.hasConsistentDaySettings() || st.day < 0 ||
            static_cast<std::size_t>(st.day) >= s.dayCount() || st.agents.size() != s.agentCount())
            return {};
        const std::int32_t limit = s.daySteps[static_cast<std::size_t>(st.day)];
        if (limit <= 0)
            return DailyPlan(st.agents.size());

        DailyPlan plan(st.agents.size());
        std::vector<std::int32_t> stocks;
        stocks.reserve(s.field.spots.size());
        for (const Spot& spot : s.field.spots)
            stocks.push_back(spot.maxStock);
        std::vector<SpotMask> acquired(st.agents.size(), 0);
        BrandMask dayBrands = 0;

        for (AgentIndex a = 0; a < st.agents.size(); ++a) {
            const Agent& agent = st.agents[a];
            if (!agent.isPatrol()) {
                plan[a].push_back(-limit);
                continue;
            }
            CellPos pos = agent.pos;
            std::int32_t fuel = agent.fuel;
            std::int32_t elapsed = 0;
            while (elapsed < limit) {
                std::size_t bestSpot = s.field.spots.size();
                PathResult bestPath;
                std::tuple<int, int, int, std::size_t> bestKey{std::numeric_limits<int>::max(),
                                                               kImpassableCost, kImpassableCost,
                                                               s.field.spots.size()};
                for (std::size_t i = 0; i < s.field.spots.size(); ++i) {
                    if (stocks[i] <= 0 || (acquired[a] & spotBit(i)) != 0)
                        continue;
                    const auto path = shortestFuelConstrainedPath(s, st, pos, s.field.spots[i].pos,
                                                                  AgentKind::Patrol, fuel);
                    if (!path.reachable || path.cost.steps > limit - elapsed)
                        continue;
                    const BrandMask brand = brandBit(s.field.spots[i].brandIndex);
                    const int priority = (previous.matchBrands & brand) == 0
                                             ? 0
                                             : ((dayBrands & brand) == 0 ? 1 : 2);
                    const auto key = std::make_tuple(priority, path.cost.steps, path.cost.fuel, i);
                    if (key < bestKey) {
                        bestKey = key;
                        bestSpot = i;
                        bestPath = path;
                    }
                }
                if (bestSpot == s.field.spots.size())
                    break;
                for (const Direction direction : bestPath.directions)
                    plan[a].push_back(toAction(direction));
                elapsed += bestPath.cost.steps;
                fuel -= bestPath.cost.fuel;
                pos = s.field.spots[bestSpot].pos;
                --stocks[bestSpot];
                acquired[a] |= spotBit(bestSpot);
                dayBrands |= brandBit(s.field.spots[bestSpot].brandIndex);
            }
            if (elapsed < limit)
                plan[a].push_back(-(limit - elapsed));
        }
        return compressWaitActions(plan);
    }
};

inline DijkstraTable brain0(const MatchSetting& setting, const MatchState& state, CellPos start) {
    return Brain0::dijkstra(setting, state, start);
}

inline PathResult brain0(const MatchSetting& setting, const MatchState& state, CellPos start,
                         CellPos goal, AgentKind kind = AgentKind::Patrol) {
    return Brain0::shortestPath(setting, state, start, goal, kind);
}

inline PathResult brain0FuelConstrained(const MatchSetting& setting, const MatchState& state,
                                        CellPos start, CellPos goal, AgentKind kind,
                                        std::int32_t fuelBudget) {
    return Brain0::shortestFuelConstrainedPath(setting, state, start, goal, kind, fuelBudget);
}

inline TimedPathResult brain0TimedRefuels(const MatchSetting& setting, const MatchState& state,
                                          CellPos start, CellPos goal, AgentKind kind,
                                          std::int32_t initialFuel, std::int32_t fuelLimit,
                                          std::int32_t startStep, std::int32_t stepLimit,
                                          const std::vector<TimedRefuel>& refuels) {
    return Brain0::shortestPathWithTimedRefuels(setting, state, start, goal, kind, initialFuel,
                                                fuelLimit, startStep, stepLimit, refuels);
}

inline DailyPlan brain0Greedy(const MatchSetting& setting, const MatchState& state,
                              ScoreState previous = {}) {
    return Brain0::greedyPlan(setting, state, previous);
}

} // namespace benzen
