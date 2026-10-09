#include "model.hpp"

#include <cassert>
#include <chrono>
#include <functional>
#include <random>
#include <vector>

using namespace benzen;

namespace {
using BrandScore = std::pair<std::uint32_t, std::uint32_t>;
BrandScore brandScore(const GameState& state) {
    return {countBits(state.score.matchBrands), countBits(state.dayBrands)};
}

// Independent oracle: enumerate complete per-agent plans geometrically, then
// take their Cartesian product and replay them. No reachability bounds/pruning.
BrandScore enumeratePlans(const MatchSetting& s, const MatchState& ms, ScoreState prev) {
    std::vector<std::vector<AgentPlan>> candidates(ms.agents.size());
    for (AgentIndex a = 0; a < ms.agents.size(); ++a) {
        AgentPlan path;
        std::function<void(CellPos, int)> enumerate = [&](CellPos pos, int left) {
            if (!left) {
                candidates[a].push_back(path);
                return;
            }
            path.push_back(-1);
            enumerate(pos, left - 1);
            path.pop_back();
            const auto cost = moveStepCost(s, ms, pos);
            if (cost > left) return;
            for (int d = 0; d < 6; ++d) {
                const auto to = s.field.neighbor(pos, static_cast<Direction>(d));
                if (!s.field.contains(to) || !s.field.at(to).isPassable()) continue;
                path.push_back(d);
                enumerate(to, left - cost);
                path.pop_back();
            }
        };
        enumerate(ms.agents[a].pos, s.daySteps[ms.day]);
    }
    BrandScore best{};
    DailyPlan plan(ms.agents.size());
    std::function<void(AgentIndex)> combine = [&](AgentIndex a) {
        if (a == plan.size()) {
            const auto replay = simulateDay(s, ms, plan, prev);
            if (replay.valid) best = std::max(best, brandScore(replay.state));
            return;
        }
        for (const auto& path : candidates[a]) {
            plan[a] = path;
            combine(a + 1);
        }
    };
    combine(0);
    return best;
}

void checkExactAgainstOracle() {
    std::mt19937 random(37);
    for (int test = 0; test < 48; ++test) {
        MatchSetting s;
        s.field.width = test % 2 ? 2 : 3;
        s.field.height = test % 2 ? 2 : 1;
        s.field.cells.assign(s.field.cellCount(), {CellType::Plain, -1});
        const auto last = static_cast<CellPos>(s.field.cellCount() - 1);
        for (auto& cell : s.field.cells)
            cell.type = static_cast<CellType>(random() % 4);
        // Include ponds, slow departure cells, duplicate brands and scarce stocks.
        s.field.cells[0].type = CellType::Plain;
        s.field.cells[last].type = CellType::Plain;
        s.field.spots = {{7, 0, 1, -1}, {static_cast<int>(random() % 2 + 7), last, 1, -1}};
        assert(s.field.rebuildSpotIndex());
        const int steps = test % 6 + 1;
        s.daySteps = {steps};
        s.daySeconds = {60};
        s.fuelLimit = test % 4;
        s.agentStarts = {0, last};
        MatchState ms;
        ms.trafficByPos.assign(s.field.cellCount(), static_cast<TrafficStatus>(test % 3));
        ms.agents = {{0, AgentKind::Patrol, 0, s.fuelLimit},
                     {1, test % 3 ? AgentKind::Supply : AgentKind::Patrol, last, s.fuelLimit}};
        ScoreState prev;
        if (test % 3 == 0) prev.matchBrands = brandBit(0);
        const auto exact = maximizeDayBrands(s, ms, prev, DailyPlan(2, AgentPlan{-steps}),
            0, std::chrono::steady_clock::time_point::max());
        assert(exact.optimal && !exact.interrupted);
        assert(brandScore(exact.finalState) == enumeratePlans(s, ms, prev));
        assert(exact.matchUpperBound == countBits(exact.finalState.score.matchBrands));
        assert(exact.dailyUpperBound == countBits(exact.finalState.dayBrands));
        const auto replay = simulateDay(s, ms, exact.plan, prev);
        assert(replay.valid && replay.state == exact.finalState);
    }
}

void checkLimitsAndIntegration() {
    MatchSetting s;
    s.field.width = 4;
    s.field.height = 1;
    s.field.cells.assign(4, {CellType::Plain, -1});
    s.field.spots = {{1, 1, 1, -1}, {2, 3, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    s.agentStarts = {0};
    s.daySteps = {6};
    s.daySeconds = {60};
    s.fuelLimit = 3;
    MatchState ms;
    ms.agents = {{0, AgentKind::Patrol, 0, 3}};
    ms.trafficByPos.assign(4, TrafficStatus::Smooth);
    DailyPlan wait{{-6}};
    const auto limited = maximizeDayBrands(s, ms, {}, wait, 1,
        std::chrono::steady_clock::time_point::max());
    assert(!limited.optimal && limited.interrupted);
    assert(simulateDay(s, ms, limited.plan).valid);
    const auto expired = maximizeDayBrands(s, ms, {}, wait, 0,
        std::chrono::steady_clock::now());
    assert(!expired.optimal && expired.interrupted && expired.plan == wait);

    BeamSearchConfig cfg;
    cfg.beamWidth = cfg.jointBeamWidth = 1;
    cfg.iterativeDeepening = false;
    cfg.useStateDeadline = false;
    cfg.exactBrands = true;
    const auto result = brain1(s, ms, {}, cfg);
    assert(result.success && result.brandOptimal);
    assert(countBits(result.finalState.dayBrands) == 2);
    assert(simulateDay(s, ms, result.plan).state == result.finalState);
    // Geometric reachability exceeds feasible fuel: prove the smaller optimum.
    ms.agents[0].fuel = 1;
    const auto fuelLimited = brain1(s, ms, {}, cfg);
    assert(fuelLimited.brandOptimal);
    assert(countBits(fuelLimited.finalState.dayBrands) == 1);
    assert(fuelLimited.dailyBrandUpperBound == 1);
    // A pond isolates an otherwise displayed brand.
    s.field.cells[2].type = CellType::Pond;
    const auto disconnected = brain1(s, ms, {}, cfg);
    assert(disconnected.brandOptimal && disconnected.dailyBrandUpperBound == 1);
}

void checkBeamMissIsRepaired() {
    MatchSetting s;
    s.field.width = 7;
    s.field.height = 1;
    s.field.cells.assign(7, {CellType::Plain, -1});
    s.field.spots = {{0, 1, 1, -1}, {1, 4, 1, -1}, {2, 5, 1, -1}, {3, 6, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    s.agentStarts = {2};
    s.daySteps = {8};
    s.daySeconds = {60};
    s.fuelLimit = 8;
    MatchState ms;
    ms.agents = {{0, AgentKind::Patrol, 2, 8}};
    ms.trafficByPos.assign(7, TrafficStatus::Smooth);
    BeamSearchConfig cfg;
    cfg.refineBrands = false;
    cfg.beamWidth = cfg.jointBeamWidth = cfg.maxActionsPerAgent = 1;
    cfg.macroTargetLimit = 1;
    cfg.adaptiveBeamWidth = cfg.adaptiveActionLimit = cfg.autoTuneWeights = false;
    cfg.coordinatedAssignments = cfg.twoSpotLookahead = false;
    cfg.comparePeriodicReplanPlans = cfg.iterativeDeepening = false;
    cfg.useStateDeadline = false;
    const auto baseline = brain1(s, ms, {}, cfg);
    cfg.refineBrands = true;
    const auto repaired = brain1(s, ms, {}, cfg);
    assert(baseline.success && repaired.success);
    assert(countBits(baseline.finalState.dayBrands) == 2);
    assert(repaired.brandOptimal && countBits(repaired.finalState.dayBrands) == 3);
    assert(brandScore(repaired.finalState) == enumeratePlans(s, ms, {}));
    assert(simulateDay(s, ms, repaired.plan).state == repaired.finalState);

    // A bounded first pass must use the remaining brand-search time instead
    // of handing an unresolved upper bound to quantity-only refiners.
    cfg.timeLimitMs = 1000;
    cfg.safetyMarginMs = 0;
    cfg.brandSearchNodeLimit = 1;
    cfg.useBrandRepair = false;
    cfg.useSpotRouteLns = false;
    cfg.useUdonAlns = false;
    const auto rescued = brain1(s, ms, {}, cfg);
    assert(rescued.success && rescued.brandOptimal);
    assert(rescued.brandSearchExpanded > cfg.brandSearchNodeLimit);
    assert(brandScore(rescued.finalState) == enumeratePlans(s, ms, {}));
    assert(simulateDay(s, ms, rescued.plan).state == rescued.finalState);
}
}  // namespace

int main() {
    checkExactAgainstOracle();
    checkLimitsAndIntegration();
    checkBeamMissIsRepaired();
}
