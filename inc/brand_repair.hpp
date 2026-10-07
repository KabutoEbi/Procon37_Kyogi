#pragma once

#include "brand_search.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <vector>

namespace benzen {

struct BrandRepairConfig {
    std::size_t maxAttempts = 16;
    std::size_t nodesPerAttempt = 1000;
    std::size_t maxPatrols = 2;
    std::size_t maxCutVariants = 2;
};

// A one-brand miss on the small competition tier dominates any quantity gain.
// Widen only that rescue neighborhood; the caller's deadline is still the
// hard bound, and other board sizes retain the established defaults.
inline BrandRepairConfig strengthenedSmallBoardBrandRepair(
    const MatchSetting& setting, const GameState& incumbent,
    BrandRepairConfig config) {
    if (setting.field.width != 16 || setting.field.height != 16 ||
        countBits(incumbent.dayBrands) >= setting.field.brands.size()) {
        return config;
    }
    config.maxAttempts = std::max<std::size_t>(config.maxAttempts, 48);
    config.nodesPerAttempt = std::max<std::size_t>(config.nodesPerAttempt, 3000);
    config.maxPatrols = std::max<std::size_t>(config.maxPatrols, 3);
    config.maxCutVariants = std::max<std::size_t>(config.maxCutVariants, 5);
    return config;
}

struct BrandRepairResult {
    DailyPlan plan;
    GameState finalState;
    std::size_t attempts = 0;
    std::size_t expanded = 0;
    std::size_t improvements = 0;
    bool interrupted = false;
};

namespace detail {
struct RepairCheckpoint {
    GameState state;
    std::array<std::size_t, kMaxAgents> cursors{};
};
struct RepairTrace {
    std::vector<RepairCheckpoint> checkpoints;
    std::vector<std::vector<int>> actionsAtStep;
    std::vector<std::vector<int>> starts;
};

// Snapshots are taken BEFORE the action phase. An in-flight action stays in
// the prefix; its exact remaining time, fuel cost and source are preserved.
inline RepairTrace repairTrace(const MatchSetting& s, const MatchState& ms,
    ScoreState prev, const DailyPlan& plan, const std::function<bool()>& stopped) {
    RepairTrace trace;
    GameState state;
    if (!state.initialize(s, ms, prev)) return trace;
    trace.actionsAtStep.assign(state.agentCount,
        std::vector<int>(state.dayStepLimit, std::numeric_limits<int>::max()));
    trace.starts.resize(state.agentCount);
    std::array<std::size_t, kMaxAgents> cursors{};
    while (state.remainingSteps() > 0) {
        if (stopped()) return {};
        trace.checkpoints.push_back({state, cursors});
        for (AgentIndex a = 0; a < state.agentCount; ++a) {
            if (state.pendingActions[a].active) continue;
            if (cursors[a] >= plan[a].size()) return {};
            const int action = plan[a][cursors[a]++];
            trace.actionsAtStep[a][state.step] = action;
            trace.starts[a].push_back(state.step);
            if (tryScheduleAction(state, s, ms, a, action).error != SimulationError::None)
                return {};
        }
        if (advanceSimulationStep(state, s).error != SimulationError::None) return {};
    }
    return trace;
}
}  // namespace detail

// Rescue missing brands by rewinding an incumbent and freeing one/two patrols
// and one supply. All other agents replay the original absolute action times.
// Local optimality is NEVER reported as a global optimality certificate.
inline BrandRepairResult repairDayBrands(
    const MatchSetting& s, const MatchState& ms, ScoreState prev,
    const DailyPlan& incumbent, const BrandRepairConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    BrandRepairResult result;
    result.plan = incumbent;
    const auto checked = simulateDay(s, ms, incumbent, prev);
    if (!checked.valid) return result;
    result.finalState = checked.state;
    const auto score = [](const GameState& state) {
        return std::make_pair(countBits(state.score.matchBrands), countBits(state.dayBrands));
    };
    const auto stopped = [&] {
        if (Clock::now() >= deadline) result.interrupted = true;
        return result.interrupted;
    };
    BrandMask all = 0;
    for (const auto& spot : s.field.spots) all |= brandBit(spot.brandIndex);
    if (result.finalState.dayBrands == all || !config.maxAttempts ||
        !config.nodesPerAttempt || !config.maxPatrols || stopped()) return result;
    const auto distances = buildBrandDistances(s, ms, stopped);
    if (distances.size() != s.field.spots.size()) return result;

    struct Target {
        int fresh;
        std::size_t owners;
        int slack;
        int brand;
    };
    // Every distance is only an optimistic bound; no fuel infeasibility is
    // inferred from it. Refuel timing is decided by the joint simulation.
    const auto distance = [&](const GameState& state, AgentIndex a, int brand) {
        int best = kImpassableCost;
        for (std::size_t i = 0; i < s.field.spots.size(); ++i) {
            if (s.field.spots[i].brandIndex != brand) continue;
            best = std::min(best, distances[i][state.agents[a].pos]);
            const auto& pending = state.pendingActions[a];
            if (pending.isMove()) best = std::min(best,
                pending.remainingSteps + distances[i][pending.destination]);
        }
        return best;
    };
    while (result.attempts < config.maxAttempts && !stopped()) {
        // Rebuild after acceptance: no stale checkpoint, stock or action index
        // from the previous incumbent is reused for the next rescue.
        const auto trace = detail::repairTrace(s, ms, prev, result.plan, stopped);
        if (trace.checkpoints.empty()) break;
        std::vector<Target> targets;
        for (std::size_t b = 0; b < s.field.brands.size(); ++b) {
            const auto bit = brandBit(b);
            if (result.finalState.dayBrands & bit) continue;
            std::size_t owners = 0;
            int slack = 0;
            for (AgentIndex a = 0; a < ms.agents.size(); ++a) {
                if (!ms.agents[a].isPatrol()) continue;
                int bestSlack = -1;
                for (const auto& cp : trace.checkpoints) {
                    const auto d = distance(cp.state, a, static_cast<int>(b));
                    if (d < kImpassableCost)
                        bestSlack = std::max(bestSlack, cp.state.remainingSteps() - std::max(1, d));
                }
                if (bestSlack >= 0) { ++owners; slack = std::max(slack, bestSlack); }
            }
            if (owners) targets.push_back({(prev.matchBrands & bit) ? 1 : 0,
                                          owners, slack, static_cast<int>(b)});
        }
        std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) {
            return std::tie(a.fresh, a.owners, a.slack, a.brand) <
                   std::tie(b.fresh, b.owners, b.slack, b.brand);
        });
        bool improved = false;
        for (const auto& target : targets) {
            std::vector<std::pair<int, AgentIndex>> patrols;
            for (AgentIndex a = 0; a < ms.agents.size(); ++a) {
                if (!ms.agents[a].isPatrol()) continue;
                patrols.emplace_back(distance(trace.checkpoints.front().state, a, target.brand), a);
            }
            std::sort(patrols.begin(), patrols.end());
            for (const auto& owner : patrols) {
                int latest = -1;
                for (const auto& cp : trace.checkpoints) {
                    const int d = distance(cp.state, owner.second, target.brand);
                    if (d < kImpassableCost && std::max(1, d) <= cp.state.remainingSteps())
                        latest = cp.state.step;
                }
                if (latest < 0) continue;
                // First a short suffix, then a wider rollback. The latter can
                // exchange responsibilities for brands originally owned by A.
                std::vector<int> cuts;
                const auto addCut = [&](int cut) {
                    cut = std::clamp(cut, 0, latest);
                    if (std::find(cuts.begin(), cuts.end(), cut) == cuts.end())
                        cuts.push_back(cut);
                };
                addCut(latest - 4);
                addCut(0);
                if (config.maxCutVariants > 2) {
                    addCut(latest - 2);
                    addCut(latest * 3 / 4);
                    addCut(latest / 2);
                }
                if (cuts.size() > config.maxCutVariants)
                    cuts.resize(config.maxCutVariants);
                for (const int cut : cuts) {
                    for (std::size_t radius = 1;
                         radius <= std::min(config.maxPatrols, patrols.size());
                         ++radius) {
                        if (radius > patrols.size()) continue;
                        if (result.attempts >= config.maxAttempts || stopped()) break;
                        const auto& cp = trace.checkpoints[cut];
                        BrandNeighborhood neighborhood;
                        neighborhood.start = cp.state;
                        neighborhood.primary = owner.second;
                        neighborhood.target = brandBit(target.brand);
                        neighborhood.preserve = result.finalState.dayBrands;
                        neighborhood.actionsAtStep = &trace.actionsAtStep;
                        neighborhood.actionStarts = &trace.starts;
                        neighborhood.freeAgents[owner.second] = true;
                        if (radius > 1) {
                            // Let the other patrols take over brands from A's
                            // abandoned suffix. Radius 3 supports a two-step
                            // responsibility chain on the four-agent tier.
                            std::vector<std::pair<int, AgentIndex>> substitutes;
                            for (const auto& other : patrols) {
                                if (other.second == owner.second) continue;
                                int cost = kImpassableCost - 1;
                                for (std::size_t i = 0; i < s.field.spots.size(); ++i) {
                                    const auto bit = brandBit(s.field.spots[i].brandIndex);
                                    if ((result.finalState.acquiredSpots[owner.second] & spotBit(i)) &&
                                        !(cp.state.dayBrands & bit))
                                        cost = std::min(cost, distances[i][cp.state.agents[other.second].pos]);
                                }
                                substitutes.emplace_back(cost, other.second);
                            }
                            std::sort(substitutes.begin(), substitutes.end());
                            if (substitutes.size() + 1 < radius) continue;
                            for (std::size_t i = 0; i + 1 < radius; ++i)
                                neighborhood.freeAgents[substitutes[i].second] = true;
                        }
                        AgentIndex supply = ms.agents.size();
                        int nearest = kImpassableCost;
                        const auto p = s.field.toCoord(cp.state.agents[owner.second].pos);
                        for (AgentIndex a = 0; a < ms.agents.size(); ++a) {
                            if (!ms.agents[a].isSupply()) continue;
                            const auto q = s.field.toCoord(cp.state.agents[a].pos);
                            const int cost = std::abs(p.x - q.x) + std::abs(p.y - q.y);
                            if (cost < nearest) { nearest = cost; supply = a; }
                        }
                        if (supply < ms.agents.size()) neighborhood.freeAgents[supply] = true;
                        neighborhood.prefix.resize(ms.agents.size());
                        for (AgentIndex a = 0; a < ms.agents.size(); ++a) {
                            neighborhood.prefix[a].assign(result.plan[a].begin(),
                                result.plan[a].begin() + cp.cursors[a]);
                            auto& pending = neighborhood.start.pendingActions[a];
                            if (neighborhood.freeAgents[a] && pending.isWait()) {
                                // A chosen long wait can be split at the cut.
                                // Committed moves cannot be interrupted.
                                const int elapsed = -neighborhood.prefix[a].back() - pending.remainingSteps;
                                if (elapsed) neighborhood.prefix[a].back() = -elapsed;
                                else neighborhood.prefix[a].pop_back();
                                pending = PendingAction{};
                            }
                            if (!neighborhood.freeAgents[a]) {
                                for (std::size_t i = 0; i < s.field.spots.size(); ++i)
                                    if (result.finalState.acquiredSpots[a] & spotBit(i))
                                        neighborhood.fixedBrands |= brandBit(s.field.spots[i].brandIndex);
                            }
                        }
                        auto attemptDeadline = deadline;
                        if (deadline != Clock::time_point::max()) {
                            const auto now = Clock::now();
                            if (now >= deadline) { result.interrupted = true; break; }
                            const auto slots = std::min<std::size_t>(4, config.maxAttempts - result.attempts);
                            attemptDeadline = now + (deadline - now) / slots;
                        }
                        ++result.attempts;
                        const auto candidate = maximizeDayBrands(s, ms, prev, result.plan,
                            config.nodesPerAttempt, attemptDeadline, &neighborhood, &distances);
                        result.expanded += candidate.expanded;
                        if (score(candidate.finalState) > score(result.finalState)) {
                            result.plan = candidate.plan;
                            result.finalState = candidate.finalState;
                            ++result.improvements;
                            improved = true;
                            break;
                        }
                    }
                    if (improved || result.attempts >= config.maxAttempts || stopped()) break;
                }
                if (improved || result.attempts >= config.maxAttempts || stopped()) break;
            }
            if (improved || result.attempts >= config.maxAttempts || stopped()) break;
        }
        if (!improved) break;
    }
    return result;
}

}  // namespace benzen
