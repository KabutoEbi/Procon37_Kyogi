#include "model.hpp"

#include <cassert>
#include <chrono>
#include <random>

using namespace benzen;

namespace {
auto brandScore(const GameState& state) {
    return std::make_pair(countBits(state.score.matchBrands), countBits(state.dayBrands));
}

MatchSetting line(int width, int steps, std::vector<CellPos> starts, std::vector<Spot> spots) {
    MatchSetting s;
    s.field.width = width;
    s.field.height = 1;
    s.field.cells.assign(width, {CellType::Plain, -1});
    s.field.spots = std::move(spots);
    assert(s.field.rebuildSpotIndex());
    s.agentStarts = std::move(starts);
    s.daySteps = {steps};
    s.daySeconds = {60};
    s.fuelLimit = steps;
    return s;
}

MatchState stateFor(const MatchSetting& s, const std::vector<AgentKind>& kinds) {
    MatchState ms;
    ms.trafficByPos.assign(s.field.cellCount(), TrafficStatus::Smooth);
    for (AgentIndex a = 0; a < kinds.size(); ++a)
        ms.agents.push_back({a, kinds[a], s.agentStarts[a], s.fuelLimit});
    return ms;
}

void checkResponsibilityChain() {
    const auto s = line(7, 8, {2, 0, 6, 6}, {{0, 1, 1, -1}, {1, 6, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol,
                               AgentKind::Supply, AgentKind::Supply});
    const DailyPlan incumbent{{5, -6}, {-8}, {-8}, {-8}};
    const auto original = simulateDay(s, ms, incumbent);
    assert(original.valid && countBits(original.state.dayBrands) == 1);
    BrandRepairConfig cfg;
    cfg.maxPatrols = 1;
    const auto alone = repairDayBrands(s, ms, {}, incumbent, cfg,
        std::chrono::steady_clock::time_point::max());
    assert(alone.improvements == 0 && alone.plan == incumbent);
    cfg.maxPatrols = 2;
    const auto chain = repairDayBrands(s, ms, {}, incumbent, cfg,
        std::chrono::steady_clock::time_point::max());
    assert(chain.improvements == 1 && countBits(chain.finalState.dayBrands) == 2);
    assert(chain.plan[3] == incumbent[3]); // Unselected supply keeps its timeline.
    const auto replay = simulateDay(s, ms, chain.plan);
    assert(replay.valid && replay.state == chain.finalState);
}

void checkWaitCutAndPendingMove() {
    auto s = line(4, 12, {0, 3}, {{0, 1, 1, -1}, {1, 3, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    const DailyPlan incumbent{{2, -10}, {-12}};
    const auto trace = detail::repairTrace(s, ms, {}, incumbent, [] { return false; });
    assert(trace.checkpoints.size() == 12);
    assert(trace.checkpoints[1].state.pendingActions[0].isMove());
    assert(trace.checkpoints[1].state.pendingActions[0].remainingSteps == 1);
    assert(trace.checkpoints[4].state.pendingActions[0].isWait());
    const auto repaired = repairDayBrands(s, ms, {}, incumbent, {},
        std::chrono::steady_clock::time_point::max());
    assert(repaired.improvements && repaired.finalState.dayBrands == 3);
    assert(simulateDay(s, ms, repaired.plan).state == repaired.finalState);
    // The first late-cut attempt should work, without reverting the whole day.
    assert(repaired.attempts == 1);

    // This late cut falls inside the first move. Its full original action
    // remains in the prefix and only the uncommitted suffix can change.
    s.daySteps[0] = 9;
    const DailyPlan movingIncumbent{{2, -7}, {-9}};
    const auto inFlight = repairDayBrands(s, ms, {}, movingIncumbent, {},
        std::chrono::steady_clock::time_point::max());
    assert(inFlight.improvements && inFlight.attempts == 1);
    assert(inFlight.plan[0].front() == 2);
    assert(simulateDay(s, ms, inFlight.plan).state == inFlight.finalState);
    s.daySteps[0] = 12;

    const auto expired = repairDayBrands(s, ms, {}, incumbent, {},
        std::chrono::steady_clock::now());
    assert(expired.interrupted && expired.attempts == 0 && expired.plan == incumbent);
    BrandRepairConfig disabled;
    disabled.maxAttempts = 0;
    assert(repairDayBrands(s, ms, {}, incumbent, disabled,
        std::chrono::steady_clock::time_point::max()).plan == incumbent);
}

void checkSupplyChain() {
    auto s = line(7, 8, {2, 0, 3, 6}, {{0, 1, 1, -1}, {1, 6, 1, -1}});
    s.fuelLimit = 4;
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol,
                           AgentKind::Supply, AgentKind::Supply});
    ms.agents[0].fuel = 1; // A must meet the supply before its second move.
    const DailyPlan incumbent{{5, -6}, {-8}, {-8}, {-8}};
    const auto repaired = repairDayBrands(s, ms, {}, incumbent, {},
        std::chrono::steady_clock::time_point::max());
    assert(repaired.improvements && repaired.finalState.dayBrands == 3);
    assert(repaired.plan[3] == incumbent[3]);
    const auto replay = simulateDay(s, ms, repaired.plan);
    assert(replay.valid && replay.state == repaired.finalState);
}

void checkRandomValidityAndNonRegression() {
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        CreateConfig create;
        create.width = create.height = 8;
        create.agentCount = 4;
        create.spotCount = 8;
        create.brandCount = 5;
        create.seed = seed;
        auto s = ProblemCreator::generate(create).setting;
        s.daySteps.resize(1);
        s.daySeconds.resize(1);
        s.daySteps[0] = 16;
        s.fuelLimit = 8;
        auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol,
                              AgentKind::Supply, AgentKind::Supply});
        for (std::size_t i = 0; i < ms.trafficByPos.size(); ++i)
            ms.trafficByPos[i] = static_cast<TrafficStatus>((i + seed) % 3);
        BeamSearchConfig cfg;
        cfg.refineBrands = false;
        cfg.beamWidth = 8;
        cfg.jointBeamWidth = 16;
        cfg.threadCount = 1;
        cfg.useStateDeadline = cfg.iterativeDeepening = false;
        cfg.comparePeriodicReplanPlans = false;
        const auto beam = brain1(s, ms, {}, cfg);
        assert(beam.success);
        BrandRepairConfig repair;
        repair.maxAttempts = 4;
        repair.nodesPerAttempt = 80;
        const auto result = repairDayBrands(s, ms, {}, beam.plan, repair,
            std::chrono::steady_clock::time_point::max());
        const auto replay = simulateDay(s, ms, result.plan);
        assert(replay.valid && replay.state == result.finalState);
        assert(brandScore(result.finalState) >= brandScore(beam.finalState));
        assert(result.attempts <= repair.maxAttempts);
        assert(result.expanded <= repair.maxAttempts * repair.nodesPerAttempt);
    }
}

void checkSmallBoardStrengthening() {
    MatchSetting s;
    s.field.width = s.field.height = 16;
    s.field.cells.assign(s.field.cellCount(), {CellType::Plain, -1});
    s.field.spots = {{0, 0, 1, -1}, {1, 255, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    GameState incumbent;
    incumbent.dayBrands = brandBit(0);
    const BrandRepairConfig ordinary;
    const auto stronger = strengthenedSmallBoardBrandRepair(s, incumbent, ordinary);
    assert(stronger.maxAttempts >= 48);
    assert(stronger.nodesPerAttempt >= 3000);
    assert(stronger.maxPatrols >= 3);
    assert(stronger.maxCutVariants >= 5);

    incumbent.dayBrands |= brandBit(1);
    const auto complete = strengthenedSmallBoardBrandRepair(s, incumbent, ordinary);
    assert(complete.maxAttempts == ordinary.maxAttempts);
    assert(complete.nodesPerAttempt == ordinary.nodesPerAttempt);
    assert(complete.maxPatrols == ordinary.maxPatrols);
    assert(complete.maxCutVariants == ordinary.maxCutVariants);

    s.field.width = 24;
    s.field.height = 24;
    incumbent.dayBrands = brandBit(0);
    const auto large = strengthenedSmallBoardBrandRepair(s, incumbent, ordinary);
    assert(large.maxAttempts == ordinary.maxAttempts);
    assert(large.nodesPerAttempt == ordinary.nodesPerAttempt);
}
}  // namespace

int main() {
    checkResponsibilityChain();
    checkWaitCutAndPendingMove();
    checkSupplyChain();
    checkRandomValidityAndNonRegression();
    checkSmallBoardStrengthening();
}
