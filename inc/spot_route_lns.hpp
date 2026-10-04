#pragma once

#include "brand_repair.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace benzen {

struct SpotRouteLnsConfig {
    // 巡回車1台あたりの公式シミュレーション回数。4つの切断位置へ配分する。
    std::size_t maxAttempts = 24;
    std::size_t maxInsertions = 3;
    bool finalDayOnly = false;
    bool preserveIntermediateState = true;
    // 系列を確保した後、複数巡回車の取得列を同時に組み替える。
    bool teamNeighborhood = true;
    std::size_t maxTeamAttempts = 320;
    std::size_t maxTeamRounds = 3;
    // 翌日位置の完全一致を外す場合も、到達系列数と燃料を検査する。
    bool allowFuturePositionChange = true;
    bool spotPortfolio = true;
    std::size_t spotPortfolioBeamWidth = 256;
    std::size_t spotPortfolioCandidatesPerDepth = 12;
    std::int32_t spotPortfolioHardnessWeight = 4;
};

struct SpotRouteLnsResult {
    DailyPlan plan;
    GameState finalState;
    std::size_t attempts = 0;
    std::size_t teamAttempts = 0;
    std::size_t improvements = 0;
    std::size_t portfolioMaxBrands = 0;
    std::size_t portfolioMaxUdon = 0;
    std::size_t portfolioMaxValidUdon = 0;
    bool interrupted = false;
};

namespace detail {

inline std::vector<std::size_t> acquiredSpotSequence(
    const RepairTrace& trace, const GameState& finalState,
    AgentIndex agent, std::size_t cut) {
    std::vector<std::size_t> result;
    SpotMask seen = trace.checkpoints[cut].state.acquiredSpots[agent];
    for (std::size_t i = cut + 1; i < trace.checkpoints.size(); ++i) {
        const SpotMask acquired = trace.checkpoints[i].state.acquiredSpots[agent];
        SpotMask added = acquired & ~seen;
        for (std::size_t spot = 0; spot < kMaxSpots; ++spot)
            if (added & spotBit(spot)) result.push_back(spot);
        seen |= added;
    }
    SpotMask added = finalState.acquiredSpots[agent] & ~seen;
    for (std::size_t spot = 0; spot < kMaxSpots; ++spot)
        if (added & spotBit(spot)) result.push_back(spot);
    return result;
}

inline bool rebuildSpotSuffix(
    const MatchSetting& setting, const MatchState& matchState,
    const RepairCheckpoint& checkpoint, AgentIndex agent,
    const std::vector<std::size_t>& visits, CellPos endAnchor,
    AgentPlan& plan, std::vector<DijkstraTable>& pathCache,
    std::vector<bool>& cached) {
    const GameState& state = checkpoint.state;
    if (state.pendingActions[agent].active) return false;
    CellPos pos = state.agents[agent].pos;
    std::int32_t fuel = state.agents[agent].fuel;
    std::int32_t left = state.remainingSteps();
    const auto appendPath = [&](CellPos goal, bool acquire,
                                CellPos& current, std::int32_t& remainingFuel,
                                std::int32_t& remainingSteps,
                                AgentPlan& output) {
        if (!cached[static_cast<std::size_t>(current)]) {
            pathCache[static_cast<std::size_t>(current)] =
                Brain0::dijkstra(setting, matchState, current);
            cached[static_cast<std::size_t>(current)] = true;
        }
        auto path = Brain0::restorePath(setting,
            pathCache[static_cast<std::size_t>(current)], goal,
            AgentKind::Patrol);
        if (path.reachable && path.cost.fuel > remainingFuel)
            path = Brain0::shortestFuelConstrainedPath(
                setting, matchState, current, goal, AgentKind::Patrol,
                remainingFuel);
        if (!path.reachable || path.cost.steps > remainingSteps) return false;
        for (const Direction direction : path.directions)
            output.push_back(static_cast<std::int32_t>(direction));
        remainingSteps -= path.cost.steps;
        remainingFuel -= path.cost.fuel;
        current = goal;
        // Staying on a spot only acquires after a reflection phase.
        if (acquire && path.cost.steps == 0) {
            if (remainingSteps <= 0) return false;
            output.push_back(-1);
            --remainingSteps;
        }
        return true;
    };
    for (const std::size_t spot : visits) {
        if (spot >= setting.field.spots.size() ||
            !appendPath(setting.field.spots[spot].pos, true,
                        pos, fuel, left, plan)) return false;
    }
    if (endAnchor != kInvalidCell &&
        !appendPath(endAnchor, false, pos, fuel, left, plan)) return false;
    if (left > 0) plan.push_back(-left);
    return true;
}

struct FutureRoutePotential {
    std::int32_t reachableBrands = 0;
    std::int32_t distanceSum = 0;
    std::int32_t patrolFuel = 0;
};

// Current traffic is the only known traffic. This is a conservative placement
// proxy, not a certificate for the next day's actual road conditions.
inline FutureRoutePotential futureRoutePotential(
    const MatchSetting& setting, const MatchState& matchState,
    const GameState& state, std::vector<DijkstraTable>& cache,
    std::vector<bool>& cached) {
    FutureRoutePotential result;
    const auto nextDay = static_cast<std::size_t>(matchState.day + 1);
    if (nextDay >= setting.dayCount()) return result;
    const int steps = setting.daySteps[nextDay];
    for (AgentIndex a = 0; a < state.agentCount; ++a) {
        if (!state.agents[a].isPatrol()) continue;
        result.patrolFuel += state.agents[a].fuel;
        const auto pos = static_cast<std::size_t>(state.agents[a].pos);
        if (!cached[pos]) {
            cache[pos] = Brain0::dijkstra(setting, matchState, state.agents[a].pos);
            cached[pos] = true;
        }
    }
    for (std::size_t brand = 0; brand < setting.field.brands.size(); ++brand) {
        int nearest = kImpassableCost;
        for (const Spot& spot : setting.field.spots) {
            if (spot.brandIndex != static_cast<int>(brand)) continue;
            for (AgentIndex a = 0; a < state.agentCount; ++a) {
                if (!state.agents[a].isPatrol()) continue;
                const auto pos = static_cast<std::size_t>(state.agents[a].pos);
                nearest = std::min(nearest,
                    cache[pos].distances[static_cast<std::size_t>(spot.pos)]);
            }
        }
        if (nearest <= steps) {
            ++result.reachableBrands;
            result.distanceSum += nearest;
        }
    }
    return result;
}

}  // namespace detail

// Try direct insertion and one-visit ruin/recreate on an incumbent spot
// sequence. Cell routes are reconstructed only after a sequence is chosen;
// the official simulator remains the feasibility oracle.
inline SpotRouteLnsResult improveSpotRoutes(
    const MatchSetting& setting, const MatchState& matchState, ScoreState previous,
    const DailyPlan& incumbent, const SpotRouteLnsConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    SpotRouteLnsResult result;
    result.plan = incumbent;
    const auto checked = simulateDay(setting, matchState, incumbent, previous);
    if (!checked.valid) return result;
    result.finalState = checked.state;
    const GameState incumbentState = checked.state;
    const bool finalDay = matchState.day >= 0 &&
        static_cast<std::size_t>(matchState.day + 1) >= setting.dayCount();
    if ((config.finalDayOnly && !finalDay) || !config.maxAttempts ||
        !config.maxInsertions) return result;
    const auto score = [](const GameState& state) {
        return std::make_tuple(countBits(state.score.matchBrands),
            countBits(state.dayBrands), state.score.totalUdon);
    };
    const auto quality = [&](const GameState& state) {
        std::int32_t fuel = 0;
        for (AgentIndex a = 0; a < state.agentCount; ++a)
            if (state.agents[a].isPatrol()) fuel += state.agents[a].fuel;
        return std::make_tuple(countBits(state.score.matchBrands),
            countBits(state.dayBrands), state.score.totalUdon,
            finalDay ? 0 : fuel);
    };
    const auto stopped = [&] {
        if (Clock::now() >= deadline) result.interrupted = true;
        return result.interrupted;
    };
    std::vector<DijkstraTable> pathCache(setting.field.cellCount());
    std::vector<bool> cached(setting.field.cellCount(), false);
    const auto incumbentFuture = !finalDay && config.allowFuturePositionChange
        ? detail::futureRoutePotential(setting, matchState, incumbentState,
                                      pathCache, cached)
        : detail::FutureRoutePotential{};
    const auto preservesFuture = [&](const GameState& candidate) {
        if (finalDay || !config.preserveIntermediateState) return true;
        if (candidate.score.matchBrands != incumbentState.score.matchBrands ||
            candidate.dayBrands != incumbentState.dayBrands ||
            candidate.agentCount != incumbentState.agentCount) return false;
        if (config.allowFuturePositionChange) {
            const auto future = detail::futureRoutePotential(
                setting, matchState, candidate, pathCache, cached);
            const int extra = candidate.score.totalUdon - incumbentState.score.totalUdon;
            return future.reachableBrands >= incumbentFuture.reachableBrands &&
                future.distanceSum <= incumbentFuture.distanceSum + 16 + 8 * extra &&
                future.patrolFuel >= incumbentFuture.patrolFuel - 20 - 30 * extra;
        }
        for (AgentIndex a = 0; a < candidate.agentCount; ++a) {
            if (candidate.agents[a].pos != incumbentState.agents[a].pos ||
                candidate.agents[a].fuel < incumbentState.agents[a].fuel)
                return false;
        }
        return true;
    };

    const auto trace = detail::repairTrace(
        setting, matchState, previous, incumbent, stopped);
    if (trace.checkpoints.empty()) return result;
    const std::array<int, 4> cutNumerators{{3, 2, 1, 0}};
    for (AgentIndex agent = 0; agent < matchState.agents.size() && !stopped(); ++agent) {
        if (!matchState.agents[agent].isPatrol()) continue;
        std::size_t agentAttempts = 0;
        for (const int numerator : cutNumerators) {
            if (agentAttempts >= config.maxAttempts || stopped()) break;
            const std::size_t cutsLeft = static_cast<std::size_t>(
                std::count_if(cutNumerators.begin(), cutNumerators.end(),
                    [&](int value) { return value <= numerator; }));
            const std::size_t cutBudget = std::max<std::size_t>(
                1, (config.maxAttempts - agentAttempts + cutsLeft - 1) / cutsLeft);
            const std::size_t cutLimit = agentAttempts + cutBudget;
            std::size_t cut = static_cast<std::size_t>(
                incumbentState.dayStepLimit * numerator / 4);
            cut = std::min(cut, trace.checkpoints.size() - 1);
            while (cut > 0 && trace.checkpoints[cut].state.pendingActions[agent].active)
                --cut;
            const auto& checkpoint = trace.checkpoints[cut];
            if (checkpoint.state.pendingActions[agent].active) continue;
            const auto original = detail::acquiredSpotSequence(
                trace, incumbentState, agent, cut);
            // 最初に既存列を壊さない挿入を試し、その後に各訪問を1件ずつ壊す。
            const std::size_t ruinCount = original.size() + 1;
            for (std::size_t ruin = 0; ruin < ruinCount; ++ruin) {
                if (agentAttempts >= cutLimit || stopped()) break;
                std::vector<std::size_t> sequence = original;
                if (ruin > 0) sequence.erase(sequence.begin() + (ruin - 1));
                DailyPlan bestPlan;
                GameState bestState{};
                bool haveCandidate = false;
                for (std::size_t insertion = 0;
                     insertion < config.maxInsertions && !stopped(); ++insertion) {
                    bool inserted = false;
                    std::vector<std::size_t> nextSequence;
                    DailyPlan nextPlan;
                    GameState nextState{};
                    for (std::size_t spot = 0;
                         spot < setting.field.spots.size() &&
                         agentAttempts < cutLimit && !stopped(); ++spot) {
                        if (checkpoint.state.acquiredSpots[agent] & spotBit(spot)) continue;
                        if (std::find(sequence.begin(), sequence.end(), spot) !=
                            sequence.end()) continue;
                        if (incumbentState.remainingStocks[spot] <= 0 &&
                            std::find(original.begin(), original.end(), spot) ==
                                original.end()) continue;
                        for (std::size_t at = 0;
                             at <= sequence.size() &&
                             agentAttempts < cutLimit; ++at) {
                            if (stopped()) break;
                            auto trialSequence = sequence;
                            trialSequence.insert(trialSequence.begin() + at, spot);
                            DailyPlan trial = incumbent;
                            trial[agent].assign(incumbent[agent].begin(),
                                incumbent[agent].begin() + checkpoint.cursors[agent]);
                            const CellPos anchor = finalDay ||
                                config.allowFuturePositionChange ? kInvalidCell :
                                incumbentState.agents[agent].pos;
                            if (!detail::rebuildSpotSuffix(setting, matchState,
                                    checkpoint, agent, trialSequence, anchor,
                                    trial[agent], pathCache, cached)) continue;
                            ++result.attempts;
                            ++agentAttempts;
                            const auto replay = simulateDay(
                                setting, matchState, trial, previous);
                            if (!replay.valid) continue;
                            if (!inserted || quality(replay.state) > quality(nextState)) {
                                inserted = true;
                                nextSequence = std::move(trialSequence);
                                nextPlan = std::move(trial);
                                nextState = replay.state;
                            }
                            if (agentAttempts >= cutLimit) break;
                        }
                    }
                    if (!inserted) break;
                    sequence = std::move(nextSequence);
                    bestPlan = std::move(nextPlan);
                    bestState = nextState;
                    haveCandidate = true;
                }
                if (haveCandidate && preservesFuture(bestState) &&
                    score(bestState) > score(incumbentState) &&
                    quality(bestState) > quality(result.finalState)) {
                    result.plan = std::move(bestPlan);
                    result.finalState = bestState;
                    result.improvements = 1;
                }
            }
        }
    }
    return result;
}

// Joint spot-level search. A move, swap, or short segment relocation changes
// two complete patrol routes at once. Full simulation handles stock contention,
// incidental pickups and refuelling by the unchanged supply route.
inline SpotRouteLnsResult improveTeamSpotRoutes(
    const MatchSetting& setting, const MatchState& matchState, ScoreState previous,
    const DailyPlan& incumbent, const SpotRouteLnsConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    SpotRouteLnsResult result;
    result.plan = incumbent;
    const auto initial = simulateDay(setting, matchState, incumbent, previous);
    if (!initial.valid) return result;
    result.finalState = initial.state;
    if (!config.teamNeighborhood || !config.maxTeamAttempts ||
        !config.maxTeamRounds) return result;
    const bool finalDay = static_cast<std::size_t>(matchState.day + 1) >=
        setting.dayCount();
    std::vector<AgentIndex> patrols;
    for (AgentIndex a = 0; a < matchState.agents.size(); ++a)
        if (matchState.agents[a].isPatrol()) patrols.push_back(a);
    if (patrols.empty()) return result;
    std::vector<DijkstraTable> pathCache(setting.field.cellCount());
    std::vector<bool> cached(setting.field.cellCount(), false);
    const auto initialFuture = !finalDay && config.allowFuturePositionChange
        ? detail::futureRoutePotential(setting, matchState, initial.state,
                                      pathCache, cached)
        : detail::FutureRoutePotential{};
    const auto score = [](const GameState& state) {
        return std::make_tuple(countBits(state.score.matchBrands),
            countBits(state.dayBrands), state.score.totalUdon);
    };
    const auto stopped = [&] {
        if (Clock::now() >= deadline) result.interrupted = true;
        return result.interrupted;
    };
    const auto preservesFuture = [&](const GameState& candidate) {
        if (finalDay || !config.preserveIntermediateState) return true;
        if (candidate.score.matchBrands != initial.state.score.matchBrands ||
            candidate.dayBrands != initial.state.dayBrands) return false;
        if (config.allowFuturePositionChange) {
            const auto future = detail::futureRoutePotential(
                setting, matchState, candidate, pathCache, cached);
            const int extra = candidate.score.totalUdon - initial.state.score.totalUdon;
            return future.reachableBrands >= initialFuture.reachableBrands &&
                future.distanceSum <= initialFuture.distanceSum + 16 + 8 * extra &&
                future.patrolFuel >= initialFuture.patrolFuel - 20 - 30 * extra;
        }
        for (const AgentIndex a : patrols)
            if (candidate.agents[a].pos != initial.state.agents[a].pos ||
                candidate.agents[a].fuel < initial.state.agents[a].fuel)
                return false;
        return true;
    };

    for (std::size_t round = 0; round < config.maxTeamRounds && !stopped(); ++round) {
        const auto trace = detail::repairTrace(
            setting, matchState, previous, result.plan, stopped);
        if (trace.checkpoints.empty()) break;
        const auto& start = trace.checkpoints.front();
        std::vector<std::vector<std::size_t>> routes(matchState.agents.size());
        for (const AgentIndex a : patrols)
            routes[a] = detail::acquiredSpotSequence(trace, result.finalState, a, 0);
        const DailyPlan basePlan = result.plan;
        const GameState baseState = result.finalState;
        DailyPlan roundPlan;
        GameState roundState{};
        bool found = false;
        std::size_t roundAttempts = 0;
        const auto tryRoutes = [&](AgentIndex first, AgentIndex second,
                                   const std::vector<std::size_t>& firstVisits,
                                   const std::vector<std::size_t>& secondVisits) {
            if (stopped() || roundAttempts >= config.maxTeamAttempts) return;
            DailyPlan trial = basePlan;
            const auto rebuild = [&](AgentIndex a,
                                     const std::vector<std::size_t>& visits) {
                trial[a].clear();
                const CellPos anchor = finalDay || config.allowFuturePositionChange
                    ? kInvalidCell : baseState.agents[a].pos;
                return detail::rebuildSpotSuffix(setting, matchState, start,
                    a, visits, anchor, trial[a], pathCache, cached);
            };
            if (!rebuild(first, firstVisits) ||
                (second != first && !rebuild(second, secondVisits))) return;
            ++roundAttempts;
            ++result.attempts;
            ++result.teamAttempts;
            const auto replay = simulateDay(setting, matchState, trial, previous);
            if (!replay.valid || score(replay.state) <= score(baseState) ||
                (found && score(replay.state) <= score(roundState))) return;
            if (!preservesFuture(replay.state)) return;
            found = true;
            roundPlan = std::move(trial);
            roundState = replay.state;
        };
        const auto budgetLeft = [&] {
            return !stopped() && roundAttempts < config.maxTeamAttempts;
        };

        // First cover untouched stock. Start with the patrols that acquired the
        // fewest spots, because they have the largest likely slack.
        auto receivers = patrols;
        std::sort(receivers.begin(), receivers.end(), [&](AgentIndex a, AgentIndex b) {
            return std::make_pair(routes[a].size(), a) <
                   std::make_pair(routes[b].size(), b);
        });
        for (const AgentIndex receiver : receivers) {
            for (std::size_t spot = 0; spot < setting.field.spots.size() && budgetLeft(); ++spot) {
                if (baseState.remainingStocks[spot] <= 0 ||
                    (baseState.acquiredSpots[receiver] & spotBit(spot))) continue;
                for (std::size_t at = 0; at <= routes[receiver].size() && budgetLeft(); ++at) {
                    auto changed = routes[receiver];
                    changed.insert(changed.begin() + at, spot);
                    tryRoutes(receiver, receiver, changed, changed);
                }
            }
        }

        // Improve the order of an existing route without changing its visits.
        // Reversal and relocation can remove large detours on short days.
        for (const AgentIndex a : receivers) {
            const auto& original = routes[a];
            for (std::size_t from = 0; from < original.size() && budgetLeft(); ++from) {
                for (std::size_t to = from + 2;
                     to <= original.size() && to <= from + 5 && budgetLeft(); ++to) {
                    auto reversed = original;
                    std::reverse(reversed.begin() + from, reversed.begin() + to);
                    tryRoutes(a, a, reversed, reversed);
                }
                for (std::size_t at = 0; at < original.size() && budgetLeft(); ++at) {
                    if (at == from) continue;
                    auto moved = original;
                    const auto visit = moved[from];
                    moved.erase(moved.begin() + from);
                    moved.insert(moved.begin() + at, visit);
                    tryRoutes(a, a, moved, moved);
                }
            }
        }

        // Move up to three consecutive visits between patrols. This can
        // redistribute a hard brand while freeing time for other pickups.
        for (const AgentIndex receiver : receivers) {
            for (const AgentIndex source : patrols) {
                if (receiver == source || !budgetLeft()) continue;
                for (std::size_t length = 1; length <= 3 && budgetLeft(); ++length) {
                    if (length > routes[source].size()) break;
                    for (std::size_t from = 0;
                         from + length <= routes[source].size() && budgetLeft(); ++from) {
                        auto shortened = routes[source];
                        const std::vector<std::size_t> segment(
                            shortened.begin() + from, shortened.begin() + from + length);
                        shortened.erase(shortened.begin() + from,
                                        shortened.begin() + from + length);
                        for (std::size_t at = 0;
                             at <= routes[receiver].size() && budgetLeft(); ++at) {
                            auto extended = routes[receiver];
                            extended.insert(extended.begin() + at,
                                segment.begin(), segment.end());
                            tryRoutes(source, receiver, shortened, extended);
                        }
                    }
                }
            }
        }

        // Exchange one visit between patrols. A pure relocation may be
        // blocked by timing, while the reciprocal exchange can be feasible.
        for (std::size_t i = 0; i < patrols.size() && budgetLeft(); ++i) {
            for (std::size_t j = i + 1; j < patrols.size() && budgetLeft(); ++j) {
                const AgentIndex a = patrols[i], b = patrols[j];
                for (std::size_t x = 0; x < routes[a].size() && budgetLeft(); ++x) {
                    for (std::size_t y = 0; y < routes[b].size() && budgetLeft(); ++y) {
                        auto ar = routes[a], br = routes[b];
                        std::swap(ar[x], br[y]);
                        tryRoutes(a, b, ar, br);
                    }
                }
            }
        }
        if (!found) break;
        result.plan = std::move(roundPlan);
        result.finalState = roundState;
        ++result.improvements;
    }
    return result;
}

// A fresh multi-patrol orienteering beam over spot visits. Transit is planned
// using today's weighted roads; the official simulator validates each complete
// candidate, including pickups along transit and the supply vehicle's route.
inline SpotRouteLnsResult searchSpotPortfolio(
    const MatchSetting& setting, const MatchState& matchState, ScoreState previous,
    const DailyPlan& incumbent, const SpotRouteLnsConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    SpotRouteLnsResult result;
    result.plan = incumbent;
    const auto original = simulateDay(setting, matchState, incumbent, previous);
    if (!original.valid) return result;
    result.finalState = original.state;
    if (!config.spotPortfolio || !config.spotPortfolioBeamWidth ||
        !config.spotPortfolioCandidatesPerDepth || Clock::now() >= deadline)
        return result;
    const bool finalDay = static_cast<std::size_t>(matchState.day + 1) >=
        setting.dayCount();
    std::vector<AgentIndex> patrols;
    for (AgentIndex a = 0; a < matchState.agents.size(); ++a)
        if (matchState.agents[a].isPatrol()) patrols.push_back(a);
    if (patrols.empty()) return result;
    const std::size_t spots = setting.field.spots.size();
    const std::size_t starts = matchState.agents.size();
    std::vector<DijkstraTable> paths(setting.field.cellCount());
    std::vector<bool> cached(setting.field.cellCount(), false);
    std::vector<CellPos> sources(starts + spots);
    for (AgentIndex a = 0; a < starts; ++a)
        sources[a] = matchState.agents[a].pos;
    for (std::size_t i = 0; i < spots; ++i)
        sources[starts + i] = setting.field.spots[i].pos;
    std::vector<std::vector<RouteCost>> costs(
        sources.size(), std::vector<RouteCost>(spots));
    for (std::size_t from = 0; from < sources.size(); ++from) {
        if (Clock::now() >= deadline) { result.interrupted = true; return result; }
        const auto pos = static_cast<std::size_t>(sources[from]);
        if (!cached[pos]) {
            paths[pos] = Brain0::dijkstra(setting, matchState, sources[from]);
            cached[pos] = true;
        }
        for (std::size_t to = 0; to < spots; ++to) {
            const auto path = Brain0::restorePath(setting, paths[pos],
                setting.field.spots[to].pos);
            if (path.reachable) costs[from][to] = path.cost;
        }
    }
    std::vector<int> brandDifficulty(setting.field.brands.size(), 0);
    for (std::size_t brand = 0; brand < brandDifficulty.size(); ++brand) {
        int nearest = kImpassableCost;
        for (const AgentIndex a : patrols)
            for (std::size_t i = 0; i < spots; ++i)
                if (setting.field.spots[i].brandIndex == static_cast<int>(brand))
                    nearest = std::min(nearest, costs[a][i].steps);
        brandDifficulty[brand] = nearest < kImpassableCost ? nearest : 0;
    }
    const int daySteps = setting.daySteps[static_cast<std::size_t>(matchState.day)];
    struct Node {
        std::array<std::array<std::uint8_t, kMaxSpots>, kMaxAgents> visits{};
        std::array<std::uint8_t, kMaxAgents> lengths{};
        std::array<SpotMask, kMaxAgents> acquired{};
        std::array<std::int32_t, kMaxAgents> locations{};
        std::array<std::int32_t, kMaxAgents> spent{};
        std::array<std::int32_t, kMaxAgents> fuel{};
        std::array<std::int32_t, kMaxSpots> stock{};
        BrandMask brands = 0;
        std::int32_t udon = 0;
        std::int32_t hardness = 0;
        std::int32_t totalTravel = 0;
    };
    Node start;
    for (AgentIndex a = 0; a < starts; ++a) {
        start.locations[a] = static_cast<int>(a);
        start.fuel[a] = matchState.agents[a].fuel;
    }
    for (std::size_t i = 0; i < spots; ++i)
        start.stock[i] = setting.field.spots[i].maxStock;
    std::vector<Node> beam{start};
    const auto rank = [&](const Node& node) {
        return std::make_tuple(countBits(node.brands), node.udon,
            config.spotPortfolioHardnessWeight * node.hardness -
                node.totalTravel);
    };
    const auto betterNode = [&](const Node& lhs, const Node& rhs) {
        return rank(lhs) > rank(rhs);
    };
    const auto officialScore = [](const GameState& state) {
        return std::make_tuple(countBits(state.score.matchBrands),
            countBits(state.dayBrands), state.score.totalUdon);
    };
    const auto initialFuture = !finalDay && config.allowFuturePositionChange
        ? detail::futureRoutePotential(setting, matchState, original.state,
                                      paths, cached)
        : detail::FutureRoutePotential{};
    GameState emptyState;
    if (!emptyState.initialize(setting, matchState, previous)) return result;
    const detail::RepairCheckpoint checkpoint{emptyState, {}};
    const auto maxDepth = std::min<std::size_t>(
        spots * patrols.size(), static_cast<std::size_t>(48));
    for (std::size_t depth = 0; depth < maxDepth && !beam.empty(); ++depth) {
        if (Clock::now() >= deadline) { result.interrupted = true; break; }
        std::vector<Node> next;
        next.reserve(beam.size() * patrols.size() * spots);
        for (const auto& node : beam) {
            if (Clock::now() >= deadline) { result.interrupted = true; break; }
            for (const AgentIndex a : patrols) {
                for (std::size_t i = 0; i < spots; ++i) {
                    if (node.stock[i] <= 0 ||
                        (node.acquired[a] & spotBit(i))) continue;
                    const auto cost = costs[static_cast<std::size_t>(node.locations[a])][i];
                    const int duration = std::max(1, cost.steps);
                    if (!cost.reachable() || node.spent[a] + duration > daySteps ||
                        cost.fuel > node.fuel[a]) continue;
                    Node child = node;
                    child.visits[a][child.lengths[a]++] = static_cast<std::uint8_t>(i);
                    child.acquired[a] |= spotBit(i);
                    child.locations[a] = static_cast<int>(starts + i);
                    child.spent[a] += duration;
                    child.fuel[a] -= cost.fuel;
                    --child.stock[i];
                    const auto brand = static_cast<std::size_t>(
                        setting.field.spots[i].brandIndex);
                    const BrandMask bit = brandBit(brand);
                    if (!(child.brands & bit))
                        child.hardness += brandDifficulty[brand];
                    child.brands |= bit;
                    ++child.udon;
                    child.totalTravel += duration;
                    next.push_back(std::move(child));
                }
            }
        }
        if (next.empty()) break;
        if (Clock::now() >= deadline) { result.interrupted = true; break; }
        std::sort(next.begin(), next.end(), betterNode);
        beam.clear();
        beam.reserve(std::min(config.spotPortfolioBeamWidth, next.size()));
        std::unordered_map<BrandMask, std::size_t> perMask;
        std::vector<bool> selected(next.size(), false);
        const auto maskLimit = std::max<std::size_t>(
            2, config.spotPortfolioBeamWidth / 32);
        for (std::size_t index = 0; index < next.size(); ++index) {
            if (beam.size() >= config.spotPortfolioBeamWidth) break;
            auto& node = next[index];
            auto& count = perMask[node.brands];
            if (count >= maskLimit) continue;
            ++count;
            selected[index] = true;
            beam.push_back(std::move(node));
        }
        for (std::size_t index = 0;
             index < next.size() && beam.size() < config.spotPortfolioBeamWidth;
             ++index) {
            if (!selected[index]) beam.push_back(std::move(next[index]));
        }
        if (beam.empty()) break;
        for (const auto& node : beam) {
            result.portfolioMaxBrands = std::max<std::size_t>(
                result.portfolioMaxBrands, countBits(node.brands));
            result.portfolioMaxUdon = std::max<std::size_t>(
                result.portfolioMaxUdon, node.udon);
        }

        const auto evaluate = std::min(config.spotPortfolioCandidatesPerDepth,
            beam.size());
        for (std::size_t index = 0; index < evaluate; ++index) {
            if (Clock::now() >= deadline) { result.interrupted = true; break; }
            const Node& node = beam[index];
            if (countBits(node.brands) < countBits(result.finalState.dayBrands))
                continue;
            DailyPlan trial = incumbent;
            bool viable = true;
            for (const AgentIndex a : patrols) {
                std::vector<std::size_t> visits;
                visits.reserve(node.lengths[a]);
                for (std::size_t k = 0; k < node.lengths[a]; ++k)
                    visits.push_back(node.visits[a][k]);
                trial[a].clear();
                const CellPos anchor = finalDay || config.allowFuturePositionChange
                    ? kInvalidCell : original.state.agents[a].pos;
                if (!detail::rebuildSpotSuffix(setting, matchState, checkpoint,
                    a, visits, anchor, trial[a], paths, cached)) {
                    viable = false;
                    break;
                }
            }
            if (!viable) continue;
            ++result.attempts;
            const auto replay = simulateDay(setting, matchState, trial, previous);
            if (replay.valid && countBits(replay.state.dayBrands) >=
                countBits(original.state.dayBrands))
                result.portfolioMaxValidUdon = std::max<std::size_t>(
                    result.portfolioMaxValidUdon, replay.state.dayUdon);
            if (!replay.valid || officialScore(replay.state) <=
                officialScore(result.finalState)) continue;
            if (!finalDay && config.preserveIntermediateState) {
                if (replay.state.score.matchBrands != original.state.score.matchBrands ||
                    replay.state.dayBrands != original.state.dayBrands) continue;
                if (config.allowFuturePositionChange) {
                    const auto future = detail::futureRoutePotential(
                        setting, matchState, replay.state, paths, cached);
                    const int extra = replay.state.score.totalUdon -
                        original.state.score.totalUdon;
                    if (future.reachableBrands < initialFuture.reachableBrands ||
                        future.distanceSum > initialFuture.distanceSum + 16 + 8 * extra ||
                        future.patrolFuel < initialFuture.patrolFuel - 20 - 30 * extra)
                        continue;
                } else {
                    bool same = true;
                    for (const AgentIndex a : patrols)
                        same = same && replay.state.agents[a].pos ==
                            original.state.agents[a].pos &&
                            replay.state.agents[a].fuel >=
                            original.state.agents[a].fuel;
                    if (!same) continue;
                }
            }
            result.plan = std::move(trial);
            result.finalState = replay.state;
            ++result.improvements;
        }
    }
    return result;
}

}  // namespace benzen
