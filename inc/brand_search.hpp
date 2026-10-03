#pragma once

#include "brain/brain0.hpp"
#include "plan_validator.hpp"
#include "search_state.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace benzen {

struct BrandSearchResult {
    DailyPlan plan;
    GameState finalState;
    bool optimal = false;
    bool interrupted = false;
    std::size_t expanded = 0;
    std::uint32_t matchUpperBound = 0;
    std::uint32_t dailyUpperBound = 0;
};

// Fixed agent kinds and one known day of traffic. No beam/action truncation.
// Every negative wait is equivalent to repeated -1 actions for simulation.
inline BrandSearchResult maximizeDayBrands(
    const MatchSetting& setting, const MatchState& matchState,
    ScoreState previousScore, const DailyPlan& incumbent,
    std::size_t nodeLimit,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    BrandSearchResult result;
    const auto checked = simulateDay(setting, matchState, incumbent, previousScore);
    GameState initial;
    if (!checked.valid || !initial.initialize(setting, matchState, previousScore))
        return result;
    result.plan = incumbent;
    result.finalState = checked.state;
    const auto score = [](const GameState& state) {
        return std::make_pair(countBits(state.score.matchBrands),
                              countBits(state.dayBrands));
    };
    auto best = score(result.finalState);
    // Safe fallback bounds if precomputation runs out of time.
    BrandMask allBrands = 0;
    for (const auto& spot : setting.field.spots)
        allBrands |= brandBit(spot.brandIndex);
    result.matchUpperBound = countBits(previousScore.matchBrands | allBrands);
    result.dailyUpperBound = countBits(allBrands);
    if (best == std::make_pair(result.matchUpperBound, result.dailyUpperBound)) {
        result.optimal = true;
        return result;
    }
    auto stopped = [&] {
        if (Clock::now() >= deadline) result.interrupted = true;
        return result.interrupted;
    };

    // Reverse shortest paths use the cost of the predecessor (departure cell).
    // Fuel and joint route conflicts are relaxed, so these remain upper bounds.
    std::vector<std::vector<std::int32_t>> distances;
    for (const auto& spot : setting.field.spots) {
        if (stopped()) return result;
        std::vector<std::int32_t> dist(setting.field.cellCount(), kImpassableCost);
        DialQueue<CellPos> queue;
        dist[spot.pos] = 0;
        queue.push(0, spot.pos);
        while (!queue.empty()) {
            if (stopped()) return result;
            const auto [cost, pos] = queue.pop();
            if (cost != dist[pos]) continue;
            for (int d = 0; d < 6; ++d) {
                const auto from = setting.field.neighbor(pos, static_cast<Direction>(d));
                if (!setting.field.contains(from) || !setting.field.at(from).isPassable())
                    continue;
                const auto edge = moveStepCost(setting, matchState, from);
                if (edge >= kImpassableCost || cost > kImpassableCost - edge) continue;
                if (cost + edge < dist[from]) {
                    dist[from] = cost + edge;
                    queue.push(dist[from], from);
                }
            }
        }
        distances.push_back(std::move(dist));
    }
    const auto upper = [&](const GameState& state) {
        BrandMask reachable = state.dayBrands;
        if (state.remainingSteps() > 0) {
            for (std::size_t s = 0; s < state.spotCount; ++s) {
                if (state.remainingStocks[s] == 0) continue;
                for (AgentIndex a = 0; a < state.agentCount; ++a) {
                    if (!state.agents[a].isPatrol() || state.hasAcquired(a, s)) continue;
                    auto steps = distances[s][state.agents[a].pos];
                    // Relax a committed move to either endpoint. This also retains
                    // acquisitions on the departure cell during a slow move.
                    const auto& pending = state.pendingActions[a];
                    if (pending.isMove())
                        steps = std::min(steps, distances[s][pending.destination]);
                    if (steps < kImpassableCost && steps <= state.remainingSteps()) {
                        reachable |= brandBit(setting.field.spots[s].brandIndex);
                        break;
                    }
                }
            }
        }
        return std::make_pair(countBits(state.score.matchBrands | reachable),
                              countBits(reachable));
    };
    const auto rootUpper = upper(initial);
    result.matchUpperBound = rootUpper.first;
    result.dailyUpperBound = rootUpper.second;
    if (best == rootUpper) {
        result.optimal = true;
        return result;
    }

    DailyPlan plan(initial.agentCount);
    std::unordered_set<GameState, GameStateHash, GameStateEqual> visited;
    bool reachedUpper = false;
    std::function<void(const GameState&)> visit;
    std::function<void(GameState, AgentIndex)> assign;
    assign = [&](GameState state, AgentIndex agent) {
        if (reachedUpper || stopped()) return;
        if (agent == state.agentCount) {
            if (advanceSimulationStep(state, setting).error == SimulationError::None)
                visit(state);
            return;
        }
        if (state.pendingActions[agent].active) {
            assign(state, agent + 1);
            return;
        }
        // Explore approaches to fresh brands first, retaining every legal move
        // and the unit wait. Supplies retain all routes, including refuel detours.
        std::vector<std::pair<int, int>> actions;
        for (int action = -1; action < 6; ++action) {
            auto next = state;
            if (tryScheduleAction(next, setting, matchState, agent, action).error !=
                SimulationError::None) continue;
            const auto pos = next.pendingActions[agent].destination;
            int priority = kImpassableCost;
            if (state.agents[agent].isPatrol()) {
                for (std::size_t s = 0; s < state.spotCount; ++s) {
                    const auto bit = brandBit(setting.field.spots[s].brandIndex);
                    if (state.remainingStocks[s] == 0 || state.hasAcquired(agent, s) ||
                        (state.dayBrands & bit)) continue;
                    const int bias = (state.score.matchBrands & bit) ? 10000 : 0;
                    priority = std::min(priority, bias + distances[s][pos]);
                }
            }
            actions.emplace_back(priority, action);
        }
        std::sort(actions.begin(), actions.end());
        for (const auto& candidate : actions) {
            if (reachedUpper || stopped()) break;
            auto next = state;
            const int action = candidate.second;
            (void)tryScheduleAction(next, setting, matchState, agent, action);
            plan[agent].push_back(action);
            assign(next, agent + 1);
            plan[agent].pop_back();
        }
    };
    visit = [&](const GameState& state) {
        if (reachedUpper || stopped() || upper(state) <= best) return;
        if (nodeLimit && result.expanded >= nodeLimit) {
            result.interrupted = true;
            return;
        }
        if (!visited.insert(canonicalSearchState(state)).second) return;
        ++result.expanded;
        if (score(state) > best) {
            DailyPlan completed = plan;
            for (AgentIndex a = 0; a < state.agentCount; ++a) {
                const int left = state.remainingSteps() -
                    state.pendingActions[a].remainingSteps;
                if (left > 0) completed[a].push_back(-left);
            }
            const auto replay = simulateDay(setting, matchState, completed, previousScore);
            if (replay.valid && score(replay.state) > best) {
                best = score(replay.state);
                result.plan = std::move(completed);
                result.finalState = replay.state;
                reachedUpper = best == rootUpper;
            }
        }
        if (state.remainingSteps() > 0 && !reachedUpper) assign(state, 0);
    };
    visit(initial);
    result.optimal = reachedUpper || !result.interrupted;
    if (result.optimal) {
        result.matchUpperBound = best.first;
        result.dailyUpperBound = best.second;
    }
    result.plan = compressWaitActions(result.plan);
    return result;
}

}  // namespace benzen
