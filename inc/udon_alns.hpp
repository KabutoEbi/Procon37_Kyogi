#pragma once

#include "brand_repair.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace benzen {

struct UdonAlnsConfig {
    std::size_t maxAttempts = 12;
    std::size_t nodesPerAttempt = 800;
    std::size_t maxPatrols = 2;
    bool finalDayOnly = true;
};

struct UdonAlnsResult {
    DailyPlan plan;
    GameState finalState;
    std::size_t attempts = 0;
    std::size_t expanded = 0;
    std::size_t improvements = 0;
    bool interrupted = false;
};

// Adaptive large-neighborhood search for the third official score component.
// The incumbent's exact match/day brand masks are constraints: a candidate is
// accepted only when both masks are unchanged and total udon strictly rises.
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
    std::array<Operator, 4> operators{{
        {1, false, 6, 0},
        {1, true, 5, 0},
        {2, false, 4, 0},
        {2, true, 3, 0},
    }};

    while (result.attempts < config.maxAttempts && !stopped()) {
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
        const std::array<int, 4> cutNumerators{{3, 2, 1, 0}};
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
        neighborhood.maximizeUdon = true;
        neighborhood.exactBrandMasks = true;
        neighborhood.actionsAtStep = &trace.actionsAtStep;
        neighborhood.actionStarts = &trace.starts;
        neighborhood.freeAgents[primary] = true;

        if (op.patrolCount > 1) {
            AgentIndex secondary = primary;
            int bestOpportunity = -1;
            for (const AgentIndex other : patrols) {
                if (other == primary) continue;
                int opportunity = 0;
                for (std::size_t spot = 0; spot < s.field.spots.size(); ++spot) {
                    if (result.finalState.remainingStocks[spot] > 0 &&
                        !(result.finalState.acquiredSpots[other] & spotBit(spot)))
                        ++opportunity;
                }
                if (opportunity > bestOpportunity) {
                    bestOpportunity = opportunity;
                    secondary = other;
                }
            }
            if (secondary != primary) neighborhood.freeAgents[secondary] = true;
        }

        if (op.freeSupply) {
            AgentIndex supply = ms.agents.size();
            int nearest = kImpassableCost;
            const auto p = s.field.toCoord(cp.state.agents[primary].pos);
            for (AgentIndex a = 0; a < ms.agents.size(); ++a) {
                if (!ms.agents[a].isSupply()) continue;
                const auto q = s.field.toCoord(cp.state.agents[a].pos);
                const int distance = std::abs(p.x - q.x) + std::abs(p.y - q.y);
                if (distance < nearest) {
                    nearest = distance;
                    supply = a;
                }
            }
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
                    if (!(result.finalState.acquiredSpots[a] & spotBit(spot)) &&
                        result.finalState.remainingStocks[spot] > 0)
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
                4, config.maxAttempts - result.attempts);
            attemptDeadline = now + (deadline - now) / slots;
        }

        ++result.attempts;
        const auto candidate = maximizeDayBrands(s, ms, prev, result.plan,
            config.nodesPerAttempt, attemptDeadline, &neighborhood, &distances);
        result.expanded += candidate.expanded;
        const bool improved =
            candidate.finalState.score.matchBrands ==
                result.finalState.score.matchBrands &&
            candidate.finalState.dayBrands == result.finalState.dayBrands &&
            candidate.finalState.score.totalUdon >
                result.finalState.score.totalUdon;
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
