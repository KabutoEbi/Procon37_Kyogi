#pragma once

#include "brain/brain0.hpp"
#include "plan_validator.hpp"
#include "search_state.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

using BrandDistances = std::vector<std::vector<std::int32_t>>;

// A local search fixes all other agents to the incumbent's action timeline.
// Prefix actions include already reserved moves, even if they finish after cut.
struct BrandNeighborhood {
    GameState start;
    DailyPlan prefix;
    std::array<bool, kMaxAgents> freeAgents{};
    AgentIndex primary = 0;
    BrandMask target = 0;
    BrandMask preserve = 0;
    BrandMask fixedBrands = 0;
    const std::vector<std::vector<int>>* actionsAtStep = nullptr;
    const std::vector<std::vector<int>>* actionStarts = nullptr;
};

inline BrandDistances buildBrandDistances(
    const MatchSetting& setting, const MatchState& matchState,
    const std::function<bool()>& stopped) {
    BrandDistances distances;
    for (const auto& spot : setting.field.spots) {
        if (stopped()) return {};
        std::vector<std::int32_t> dist(setting.field.cellCount(), kImpassableCost);
        DialQueue<CellPos> queue;
        dist[spot.pos] = 0;
        queue.push(0, spot.pos);
        while (!queue.empty()) {
            if (stopped()) return {};
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
    return distances;
}

// Fixed agent kinds and one known day of traffic. No beam/action truncation.
// Every negative wait is equivalent to repeated -1 actions for simulation.
inline BrandSearchResult maximizeDayBrands(
    const MatchSetting& setting, const MatchState& matchState,
    ScoreState previousScore, const DailyPlan& incumbent,
    std::size_t nodeLimit,
    std::chrono::steady_clock::time_point deadline,
    const BrandNeighborhood* neighborhood = nullptr,
    const BrandDistances* sharedDistances = nullptr) {
    using Clock = std::chrono::steady_clock;
    BrandSearchResult result;
    const auto checked = simulateDay(setting, matchState, incumbent, previousScore);
    GameState initial;
    if (!checked.valid || !initial.initialize(setting, matchState, previousScore))
        return result;
    result.plan = incumbent;
    result.finalState = checked.state;
    if (neighborhood) initial = neighborhood->start;
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
    BrandDistances ownedDistances;
    if (!sharedDistances) ownedDistances = buildBrandDistances(setting, matchState, stopped);
    const auto& distances = sharedDistances ? *sharedDistances : ownedDistances;
    if (distances.size() != setting.field.spots.size()) return result;
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

    DailyPlan plan = neighborhood ? neighborhood->prefix : DailyPlan(initial.agentCount);
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
        if (neighborhood && !neighborhood->freeAgents[agent]) {
            const int action = (*neighborhood->actionsAtStep)[agent][state.step];
            if (tryScheduleAction(state, setting, matchState, agent, action).error !=
                SimulationError::None) return;
            plan[agent].push_back(action);
            assign(state, agent + 1);
            plan[agent].pop_back();
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
                    int bias = (state.score.matchBrands & bit) ? 10000 : 0;
                    if (neighborhood) {
                        const bool rescue = agent == neighborhood->primary &&
                            (state.dayBrands & neighborhood->target) == 0;
                        bias = rescue ? ((bit & neighborhood->target) ? 0 : 100000) :
                            ((bit & neighborhood->preserve) ? 0 : 10000);
                    }
                    priority = std::min(priority, bias + distances[s][pos]);
                }
            } else if (neighborhood) {
                // Let the supply approach the rescue car. A unit
                // wait at a shared cell preserves the next refuel opportunity.
                const auto& patrol = state.agents[neighborhood->primary];
                const auto coord = setting.field.toCoord(pos);
                const auto patrolCoord = setting.field.toCoord(patrol.pos);
                priority = 4 * (std::abs(coord.x - patrolCoord.x) +
                                std::abs(coord.y - patrolCoord.y));
                if (pos == patrol.pos && action == -1) priority = -1;
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
        const auto possibleCompletion = neighborhood ? std::make_pair(
            countBits(state.score.matchBrands | neighborhood->fixedBrands),
            countBits(state.dayBrands | neighborhood->fixedBrands)) : score(state);
        if (possibleCompletion > best) {
            DailyPlan completed = plan;
            for (AgentIndex a = 0; a < state.agentCount; ++a) {
                if (neighborhood && !neighborhood->freeAgents[a]) {
                    const auto& starts = (*neighborhood->actionStarts)[a];
                    const auto cursor = static_cast<std::size_t>(
                        std::lower_bound(starts.begin(), starts.end(), state.step) - starts.begin());
                    completed[a].insert(completed[a].end(), incumbent[a].begin() + cursor,
                                        incumbent[a].end());
                    continue;
                }
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
