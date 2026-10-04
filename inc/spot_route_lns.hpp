#pragma once

#include "brand_repair.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <tuple>
#include <vector>

namespace benzen {

struct SpotRouteLnsConfig {
    // 巡回車1台あたりの公式シミュレーション回数。4つの切断位置へ配分する。
    std::size_t maxAttempts = 24;
    std::size_t maxInsertions = 3;
    bool finalDayOnly = false;
    bool preserveIntermediateState = true;
};

struct SpotRouteLnsResult {
    DailyPlan plan;
    GameState finalState;
    std::size_t attempts = 0;
    std::size_t improvements = 0;
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
    const auto preservesFuture = [&](const GameState& candidate) {
        if (finalDay || !config.preserveIntermediateState) return true;
        if (candidate.score.matchBrands != incumbentState.score.matchBrands ||
            candidate.dayBrands != incumbentState.dayBrands ||
            candidate.agentCount != incumbentState.agentCount) return false;
        for (AgentIndex a = 0; a < candidate.agentCount; ++a)
            if (candidate.agents[a].pos != incumbentState.agents[a].pos ||
                candidate.agents[a].fuel < incumbentState.agents[a].fuel)
                return false;
        return true;
    };

    const auto trace = detail::repairTrace(
        setting, matchState, previous, incumbent, stopped);
    if (trace.checkpoints.empty()) return result;
    std::vector<DijkstraTable> pathCache(setting.field.cellCount());
    std::vector<bool> cached(setting.field.cellCount(), false);
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
                            const CellPos anchor = finalDay ? kInvalidCell :
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

}  // namespace benzen
