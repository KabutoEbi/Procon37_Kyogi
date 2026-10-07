#pragma once

#include "spot_route_lns.hpp"
#include <tuple>

namespace benzen {

struct OvernightResult {
    DailyPlan plan;
    GameState state;
    std::size_t attempts = 0;
    std::size_t improvements = 0;
};

// Reuse only the tail of today's plan. A trial is accepted only after full
// replay, with today's brand masks and quantity preserved. Tomorrow's traffic
// is not known: the fuel-aware placement score is deliberately only a proxy.
inline OvernightResult improveOvernightPosition(
    const MatchSetting& s, const MatchState& ms, ScoreState previous,
    const DailyPlan& incumbent, std::chrono::steady_clock::time_point deadline,
    std::size_t maxAttempts = 512) {
    using Clock = std::chrono::steady_clock;
    OvernightResult result;
    result.plan = incumbent;
    const auto replay = simulateDay(s, ms, incumbent, previous);
    result.state = replay.state;
    if (!replay.valid || ms.day < 0 ||
        static_cast<std::size_t>(ms.day + 1) >= s.dayCount()) return result;
    const auto stopped = [&] { return Clock::now() >= deadline || result.attempts >= maxAttempts; };
    if (stopped()) return result;
    std::vector<DijkstraTable> paths(s.field.cellCount()), futurePaths(s.field.cellCount());
    std::vector<bool> cached(s.field.cellCount(), false), futureCached(s.field.cellCount(), false);
    const auto potential = [&](const GameState& state) {
        return detail::futureRoutePotential(s, ms, state, futurePaths, futureCached);
    };
    const auto rank = [](const detail::FutureRoutePotential& p) {
        return std::make_tuple(p.reachableBrands, -p.strandedPatrols,
                              p.readyBrands, p.readyUdon, -p.distanceSum, p.patrolFuel);
    };
    auto bestFuture = potential(result.state);
    for (int round = 0; round < 2 && !stopped(); ++round) {
        const auto base = result.plan;
        const auto trace = detail::repairTrace(s, ms, previous, base, stopped);
        if (trace.checkpoints.empty()) break;
        const auto oldImprovements = result.improvements;
        std::vector<CellPos> goals;
        for (const auto& spot : s.field.spots) goals.push_back(spot.pos);
        for (AgentIndex a = 0; a < result.state.agentCount; ++a)
            goals.push_back(result.state.agents[a].pos);
        std::sort(goals.begin(), goals.end());
        goals.erase(std::unique(goals.begin(), goals.end()), goals.end());
        const int steps = result.state.dayStepLimit;
        std::vector<int> cuts{steps / 2, steps * 3 / 4, std::max(0, steps - 8), 0};
        const auto consider = [&](const DailyPlan& trial) {
            if (stopped()) return;
            ++result.attempts;
            const auto checked = simulateDay(s, ms, trial, previous);
            if (!checked.valid || checked.state.dayUdon < result.state.dayUdon ||
                (checked.state.dayBrands & result.state.dayBrands) != result.state.dayBrands ||
                (checked.state.score.matchBrands & result.state.score.matchBrands) != result.state.score.matchBrands)
                return;
            const auto future = potential(checked.state);
            if (future.reachableBrands < bestFuture.reachableBrands ||
                future.strandedPatrols > bestFuture.strandedPatrols) return;
            const auto score = [&](const GameState& state, const detail::FutureRoutePotential& p) {
                return std::make_tuple(countBits(state.score.matchBrands),
                    countBits(state.dayBrands), state.dayUdon, rank(p));
            };
            if (score(checked.state, future) <= score(result.state, bestFuture)) return;
            result.plan = trial;
            result.state = checked.state;
            bestFuture = future;
            ++result.improvements;
        };
        for (const int cut : cuts) {
            if (cut >= steps || stopped()) continue;
            const auto& cp = trace.checkpoints[static_cast<std::size_t>(cut)];
            // Truncate a wait at the cut, but never interrupt an in-flight move.
            const auto suffix = [&](AgentIndex a, CellPos goal, AgentPlan& actions) {
                const auto& pending = cp.state.pendingActions[a];
                if (pending.isMove()) return false;
                actions.assign(base[a].begin(), base[a].begin() + cp.cursors[a]);
                if (pending.isWait()) {
                    const int elapsed = -actions.back() - pending.remainingSteps;
                    if (elapsed) actions.back() = -elapsed;
                    else actions.pop_back();
                }
                const auto pos = cp.state.agents[a].pos;
                if (!cached[pos]) {
                    paths[pos] = Brain0::dijkstra(s, ms, pos, stopped);
                    if (stopped()) return false;
                    cached[pos] = true;
                }
                const auto path = Brain0::restorePath(s, paths[pos], goal, cp.state.agents[a].kind);
                if (!path.reachable || path.cost.steps > steps - cut) return false;
                // Full replay checks intermediate refuels and all other vehicles.
                for (const auto d : path.directions) actions.push_back(static_cast<int>(d));
                if (cut + path.cost.steps < steps) actions.push_back(cut + path.cost.steps - steps);
                return true;
            };
            for (AgentIndex a = 0; a < cp.state.agentCount && !stopped(); ++a) {
                for (const auto goal : goals) {
                    if (stopped()) break;
                    auto trial = base;
                    if (!suffix(a, goal, trial[a])) continue;
                    consider(trial);
                    if (!cp.state.agents[a].isPatrol()) continue;
                    // Joint relocation: arrive independently, then refuel on
                    // the same reflection step. Replay enforces that ordering.
                    for (AgentIndex q = 0; q < cp.state.agentCount && !stopped(); ++q) {
                        if (!cp.state.agents[q].isSupply()) continue;
                        auto joint = trial;
                        if (suffix(q, goal, joint[q])) consider(joint);
                    }
                }
            }
        }
        if (oldImprovements == result.improvements) break;
    }
    return result;
}

} // namespace benzen
