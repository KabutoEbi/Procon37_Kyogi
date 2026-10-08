#pragma once

#include "spot_route_lns.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace benzen {

namespace detail {

inline AgentIndex nearestSupplyByTravel(
    const MatchSetting& setting, const MatchState& matchState,
    const GameState& state, AgentIndex patrol,
    std::vector<DijkstraTable>& cache, std::vector<bool>& cached) {
    if (patrol >= state.agentCount ||
        cache.size() != setting.field.cellCount() ||
        cached.size() != setting.field.cellCount()) return state.agentCount;
    const CellPos goal = state.agents[patrol].pos;
    AgentIndex best = state.agentCount;
    std::int32_t bestArrival = kImpassableCost;
    for (AgentIndex supply = 0; supply < state.agentCount; ++supply) {
        if (!state.agents[supply].isSupply()) continue;
        const CellPos start = state.agents[supply].pos;
        const auto index = static_cast<std::size_t>(start);
        if (!cached[index]) {
            cache[index] = Brain0::dijkstra(setting, matchState, start);
            cached[index] = true;
        }
        const auto arrival = cache[index].distances[
            static_cast<std::size_t>(goal)];
        if (arrival >= kImpassableCost) continue;
        if (std::make_pair(arrival, supply) <
            std::make_pair(bestArrival, best)) {
            bestArrival = arrival;
            best = supply;
        }
    }
    return best;
}

} // namespace detail

struct UdonAlnsConfig {
    std::size_t maxAttempts = 12;
    // Once a useful neighborhood is found, spend a small extra budget there.
    // Unproductive instances still stop at maxAttempts.
    std::size_t improvementBonusAttempts = 4;
    std::size_t nodesPerAttempt = 800;
    std::size_t maxPatrols = 2;
    bool reallocateStock = true;
    // 中間日は系列マスクを固定し、終端位置・燃料を翌日到達性で制限する。
    bool finalDayOnly = false;
    bool preserveIntermediateState = true;
    bool allowFuturePositionChange = true;
    // 最終日は従来近傍と広い近傍を比較し、公式得点が高い方だけを採る。
    bool expandedPortfolio = true;
};

struct UdonAlnsResult {
    DailyPlan plan;
    GameState finalState;
    std::size_t attempts = 0;
    std::size_t expanded = 0;
    std::size_t improvements = 0;
    bool interrupted = false;
};

// Adaptive large-neighborhood search in official lexicographic score order.
// The incumbent's match/day brand counts are lower bounds. This permits a
// different brand combination when it keeps both counts and gains more udon.
inline UdonAlnsResult improveDayUdon(
    const MatchSetting& s, const MatchState& ms, ScoreState prev,
    const DailyPlan& incumbent, const UdonAlnsConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    UdonAlnsResult result;
    result.plan = incumbent;
    const auto checked = simulateDay(s, ms, incumbent, prev);
    if (!checked.valid) return result;
    result.finalState = checked.state;
    const bool finalDay = ms.day >= 0 &&
        static_cast<std::size_t>(ms.day + 1) >= s.dayCount();
    std::vector<DijkstraTable> futureCache(s.field.cellCount());
    std::vector<bool> futureCached(s.field.cellCount(), false);
    std::vector<DijkstraTable> supplyDistanceCache(s.field.cellCount());
    std::vector<bool> supplyDistanceCached(s.field.cellCount(), false);
    const auto initialFuture = !finalDay && config.allowFuturePositionChange
        ? detail::futureRoutePotential(s, ms, result.finalState,
                                      futureCache, futureCached)
        : detail::FutureRoutePotential{};
    const auto preservesFuture = [&](const GameState& candidate) {
        if (finalDay || !config.preserveIntermediateState) return true;
        if (candidate.score.matchBrands != result.finalState.score.matchBrands ||
            candidate.dayBrands != result.finalState.dayBrands ||
            candidate.agentCount != result.finalState.agentCount) {
            return false;
        }
        if (config.allowFuturePositionChange) {
            const auto future = detail::futureRoutePotential(
                s, ms, candidate, futureCache, futureCached);
            const int extra = candidate.score.totalUdon - result.finalState.score.totalUdon;
            return future.reachableBrands >= initialFuture.reachableBrands &&
                future.strandedPatrols <= initialFuture.strandedPatrols &&
                future.distanceSum <= initialFuture.distanceSum + 16 + 8 * extra &&
                future.patrolFuel >= initialFuture.patrolFuel - 20 - 30 * extra;
        }
        for (AgentIndex a = 0; a < candidate.agentCount; ++a) {
            if (candidate.agents[a].pos != result.finalState.agents[a].pos ||
                candidate.agents[a].fuel != result.finalState.agents[a].fuel) {
                return false;
            }
        }
        return true;
    };
    const auto stopped = [&] {
        if (Clock::now() >= deadline) result.interrupted = true;
        return result.interrupted;
    };
    if (!config.maxAttempts || !config.nodesPerAttempt ||
        !config.maxPatrols || stopped()) return result;

    std::vector<AgentIndex> patrols;
    for (AgentIndex a = 0; a < ms.agents.size(); ++a)
        if (ms.agents[a].isPatrol()) patrols.push_back(a);
    if (patrols.empty()) return result;

    const auto distances = buildBrandDistances(s, ms, stopped);
    if (distances.size() != s.field.spots.size()) return result;

    struct Operator {
        std::size_t patrolCount;
        bool freeSupply;
        int weight;
        std::size_t uses = 0;
    };
    std::array<Operator, 6> operators{{
        {1, false, 6, 0},
        {1, true, 5, 0},
        {2, false, 4, 0},
        {2, true, 3, 0},
        {3, false, 2, 0},
        {3, true, 2, 0},
    }};

    const auto attemptLimit = [&] {
        return config.maxAttempts +
            (result.improvements ? config.improvementBonusAttempts : 0);
    };
    while (result.attempts < attemptLimit() && !stopped()) {
        const auto trace = detail::repairTrace(s, ms, prev, result.plan, stopped);
        if (trace.checkpoints.empty()) break;

        std::size_t opIndex = 0;
        std::int64_t bestPriority = std::numeric_limits<std::int64_t>::min();
        for (std::size_t i = 0; i < operators.size(); ++i) {
            const auto& op = operators[i];
            if (op.patrolCount > config.maxPatrols || op.patrolCount > patrols.size())
                continue;
            const std::int64_t priority =
                static_cast<std::int64_t>(op.weight) * 1024 /
                static_cast<std::int64_t>(op.uses + 1);
            if (priority > bestPriority) {
                bestPriority = priority;
                opIndex = i;
            }
        }
        auto& op = operators[opIndex];
        ++op.uses;

        const AgentIndex primary = patrols[
            (result.attempts + opIndex) % patrols.size()];
        // Joint reallocation needs an early cut before either car consumes stock.
        const std::array<int, 4> cutNumerators = config.reallocateStock && op.patrolCount > 1
            ? std::array<int, 4>{{0, 2, 3, 1}}
            : std::array<int, 4>{{3, 2, 1, 0}};
        const int cutStep = result.finalState.dayStepLimit *
            cutNumerators[(op.uses - 1) % cutNumerators.size()] / 4;
        const auto& cp = trace.checkpoints[static_cast<std::size_t>(std::min(
            cutStep, static_cast<int>(trace.checkpoints.size() - 1)))];

        BrandNeighborhood neighborhood;
        neighborhood.start = cp.state;
        neighborhood.primary = primary;
        neighborhood.preserve = result.finalState.dayBrands;
        neighborhood.requiredMatchBrands = result.finalState.score.matchBrands;
        neighborhood.requiredDayBrands = result.finalState.dayBrands;
        neighborhood.requiredMatchBrandCount =
            countBits(result.finalState.score.matchBrands);
        neighborhood.requiredDayBrandCount = countBits(result.finalState.dayBrands);
        neighborhood.maximizeUdon = true;
        neighborhood.preserveBrandCounts = finalDay ||
            !config.preserveIntermediateState;
        neighborhood.exactBrandMasks = !neighborhood.preserveBrandCounts;
        neighborhood.actionsAtStep = &trace.actionsAtStep;
        neighborhood.actionStarts = &trace.starts;
        neighborhood.freeAgents[primary] = true;
        neighborhood.jointSupplyPriorities = config.reallocateStock;

        const auto& targetState = config.reallocateStock ? cp.state : result.finalState;

        if (op.patrolCount > 1) {
            std::vector<std::pair<int, AgentIndex>> opportunities;
            for (const AgentIndex other : patrols) {
                if (other == primary) continue;
                int opportunity = 0;
                for (std::size_t spot = 0; spot < s.field.spots.size(); ++spot) {
                    if (targetState.remainingStocks[spot] > 0 &&
                        !(targetState.acquiredSpots[other] & spotBit(spot)))
                        ++opportunity;
                }
                opportunities.emplace_back(-opportunity, other);
            }
            std::sort(opportunities.begin(), opportunities.end());
            for (std::size_t i = 0;
                 i < opportunities.size() && i + 1 < op.patrolCount; ++i) {
                neighborhood.freeAgents[opportunities[i].second] = true;
            }
        }

        if (op.freeSupply) {
            const AgentIndex supply = detail::nearestSupplyByTravel(
                s, ms, cp.state, primary,
                supplyDistanceCache, supplyDistanceCached);
            if (supply < ms.agents.size()) neighborhood.freeAgents[supply] = true;
        }

        neighborhood.prefix.resize(ms.agents.size());
        for (AgentIndex a = 0; a < ms.agents.size(); ++a) {
            neighborhood.prefix[a].assign(result.plan[a].begin(),
                result.plan[a].begin() + cp.cursors[a]);
            auto& pending = neighborhood.start.pendingActions[a];
            if (neighborhood.freeAgents[a] && pending.isWait()) {
                const int elapsed = -neighborhood.prefix[a].back() -
                    pending.remainingSteps;
                if (elapsed) neighborhood.prefix[a].back() = -elapsed;
                else neighborhood.prefix[a].pop_back();
                pending = PendingAction{};
            }
            if (neighborhood.freeAgents[a] && ms.agents[a].isPatrol()) {
                for (std::size_t spot = 0; spot < s.field.spots.size(); ++spot) {
                    // start already contains the stock at the cut: do not add
                    // stock again. Freed suffix acquisitions can change owner.
                    if (!(targetState.acquiredSpots[a] & spotBit(spot)) &&
                        targetState.remainingStocks[spot] > 0)
                        neighborhood.targetSpots |= spotBit(spot);
                }
            } else if (!neighborhood.freeAgents[a]) {
                for (std::size_t spot = 0; spot < s.field.spots.size(); ++spot) {
                    if (result.finalState.acquiredSpots[a] & spotBit(spot))
                        neighborhood.fixedBrands |=
                            brandBit(s.field.spots[spot].brandIndex);
                }
                neighborhood.fixedFutureUdon += static_cast<std::int32_t>(countBits(
                    result.finalState.acquiredSpots[a] &
                    ~cp.state.acquiredSpots[a]));
            }
        }

        auto attemptDeadline = deadline;
        if (deadline != Clock::time_point::max()) {
            const auto now = Clock::now();
            if (now >= deadline) {
                result.interrupted = true;
                break;
            }
            const auto slots = std::min<std::size_t>(
                4, attemptLimit() - result.attempts);
            attemptDeadline = now + (deadline - now) / slots;
        }

        ++result.attempts;
        const auto candidate = maximizeDayBrands(s, ms, prev, result.plan,
            config.nodesPerAttempt, attemptDeadline, &neighborhood, &distances);
        result.expanded += candidate.expanded;
        const auto officialScore = [](const GameState& state) {
            return std::make_tuple(
                countBits(state.score.matchBrands),
                countBits(state.dayBrands),
                state.score.totalUdon);
        };
        const bool improved = preservesFuture(candidate.finalState) &&
            officialScore(candidate.finalState) > officialScore(result.finalState);
        if (improved) {
            result.plan = candidate.plan;
            result.finalState = candidate.finalState;
            ++result.improvements;
            op.weight += 4;
        } else {
            op.weight = std::max(1, op.weight - 1);
        }
    }
    return result;
}

}  // namespace benzen
