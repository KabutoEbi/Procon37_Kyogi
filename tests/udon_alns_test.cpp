#include "model.hpp"

#include <cassert>
#include <chrono>
#include <random>

using namespace benzen;

namespace {
MatchSetting line(int width, int steps, std::vector<CellPos> starts,
                  std::vector<Spot> spots) {
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

MatchState stateFor(const MatchSetting& s,
                    const std::vector<AgentKind>& kinds) {
    MatchState ms;
    ms.trafficByPos.assign(s.field.cellCount(), TrafficStatus::Smooth);
    for (AgentIndex a = 0; a < kinds.size(); ++a)
        ms.agents.push_back({a, kinds[a], s.agentStarts[a], s.fuelLimit});
    return ms;
}

void checkSameBrandExtraUdon() {
    const auto s = line(4, 8, {0}, {{0, 1, 1, -1}, {0, 3, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol});
    const DailyPlan incumbent{{2, -6}};
    const auto before = simulateDay(s, ms, incumbent);
    assert(before.valid && before.state.dayUdon == 1 && before.state.dayBrands == 1);

    UdonAlnsConfig cfg;
    cfg.maxAttempts = 12;
    cfg.nodesPerAttempt = 2000;
    const auto improved = improveDayUdon(s, ms, {}, incumbent, cfg,
        std::chrono::steady_clock::time_point::max());
    assert(improved.improvements > 0);
    assert(improved.finalState.dayBrands == before.state.dayBrands);
    assert(improved.finalState.score.matchBrands == before.state.score.matchBrands);
    assert(improved.finalState.dayUdon == 2);
    const auto replay = simulateDay(s, ms, improved.plan);
    assert(replay.valid && replay.state == improved.finalState);
}

void checkRandomValidityAndFixedBrands() {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        CreateConfig create;
        create.width = create.height = 8;
        create.agentCount = 4;
        create.spotCount = 8;
        create.brandCount = 4;
        create.seed = seed;
        auto s = ProblemCreator::generate(create).setting;
        s.daySteps.resize(1);
        s.daySeconds.resize(1);
        s.daySteps[0] = 18;
        auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol,
                              AgentKind::Supply, AgentKind::Supply});
        for (std::size_t i = 0; i < ms.trafficByPos.size(); ++i)
            ms.trafficByPos[i] = static_cast<TrafficStatus>((i + seed) % 3);
        BeamSearchConfig beam;
        beam.refineBrands = false;
        beam.useUdonAlns = false;
        beam.beamWidth = 8;
        beam.jointBeamWidth = 16;
        beam.threadCount = 1;
        beam.useStateDeadline = beam.iterativeDeepening = false;
        beam.comparePeriodicReplanPlans = false;
        const auto incumbent = brain1(s, ms, {}, beam);
        assert(incumbent.success);

        UdonAlnsConfig cfg;
        cfg.maxAttempts = 6;
        cfg.nodesPerAttempt = 120;
        const auto result = improveDayUdon(s, ms, {}, incumbent.plan, cfg,
            std::chrono::steady_clock::time_point::max());
        const auto replay = simulateDay(s, ms, result.plan);
        assert(replay.valid && replay.state == result.finalState);
        assert(result.finalState.dayBrands == incumbent.finalState.dayBrands);
        assert(result.finalState.score.matchBrands ==
               incumbent.finalState.score.matchBrands);
        assert(result.finalState.score.totalUdon >=
               incumbent.finalState.score.totalUdon);
        assert(result.attempts <= cfg.maxAttempts);
        assert(result.expanded <= cfg.maxAttempts * cfg.nodesPerAttempt);
    }
}
}  // namespace

int main() {
    checkSameBrandExtraUdon();
    checkRandomValidityAndFixedBrands();
}
