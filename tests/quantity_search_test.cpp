#include "application.hpp"
#include "solver_options.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace benzen;
using Clock = std::chrono::steady_clock;

MatchSetting line() {
    MatchSetting s;
    s.field.width = 5;
    s.field.height = 1;
    s.field.cells.assign(5, {CellType::Plain, -1});
    s.field.spots = {{0, 0, 2, -1}, {0, 2, 2, -1}, {0, 4, 2, -1}};
    assert(s.field.rebuildSpotIndex());
    s.agentStarts = {0, 2};
    s.daySteps = {8};
    s.daySeconds = {60};
    s.fuelLimit = 2;
    return s;
}

void checkTransitAndTimedRefuel() {
    const auto s = line();
    MatchState ms;
    ms.agents = {{0, AgentKind::Patrol, 0, 2}, {1, AgentKind::Supply, 2, 2}};
    ms.trafficByPos.assign(5, TrafficStatus::Smooth);
    const DailyPlan plan{{2, 2, 2, 2}, {-8}};
    const auto replay = simulateDay(s, ms, plan);
    assert(replay.valid && replay.state.dayUdon == 3);
    auto trace = detail::repairTrace(s, ms, {}, plan, [] { return false; });
    trace.checkpoints.push_back({replay.state, {}});
    CellPos pos = 0;
    int fuel = 2, time = 0;
    SpotMask acquired = 0;
    const std::vector<Direction> moves(4, Direction::Right);
    assert(detail::walkPatrolPath(s, ms, moves, pos, fuel, time, &trace,
        [&](CellPos cell) {
            const int spot = s.field.at(cell).spotIndex;
            if (spot >= 0) acquired |= spotBit(spot);
        }));
    assert(acquired == replay.state.acquiredSpots[0]);
    assert(pos == replay.state.agents[0].pos && fuel == replay.state.agents[0].fuel);
    assert(time == 8);
    pos = 0; fuel = 2; time = 0;
    assert(!detail::walkPatrolPath(s, ms, moves, pos, fuel, time, nullptr, [](CellPos) {}));

    const DailyPlan incumbent{{-8}, {-8}};
    const auto improved = searchSpotPortfolio(s, ms, {}, incumbent, {}, Clock::time_point::max());
    assert(improved.finalState.dayUdon == 3);
    const auto checked = simulateDay(s, ms, improved.plan);
    assert(checked.valid && checked.state == improved.finalState);
}

void checkDayBeam() {
    auto s = line();
    s.field.spots = {{0, 0, 1, -1}, {0, 3, 1, -1}, {0, 4, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    s.agentStarts = {2};
    s.fuelLimit = 20;
    s.daySteps = {4, 4, 4};
    s.daySeconds = {60, 60, 60};
    MatchState ms;
    ms.agents = {{0, AgentKind::Patrol, 2, 20}};
    ms.trafficByPos.assign(5, TrafficStatus::Smooth);
    std::vector<DayPlanCandidate> roots;
    for (const DailyPlan& plan : {DailyPlan{{5, 5}}, DailyPlan{{2, -2}}}) {
        const auto replay = simulateDay(s, ms, plan);
        assert(replay.valid && replay.state.dayUdon == 1);
        roots.push_back({plan, replay.state});
    }
    std::size_t calls = 0;
    const auto search = [&](const MatchState& tomorrow, ScoreState score, int,
                            Clock::time_point end) {
        assert(tomorrow.endsAt == 0 && tomorrow.day > ms.day);
        assert(Clock::now() < end);
        ++calls;
        BeamSearchConfig cfg;
        cfg.beamWidth = 16;
        cfg.jointBeamWidth = 32;
        cfg.threadCount = 1;
        cfg.timeLimitMs = 20;
        cfg.safetyMarginMs = 0;
        cfg.useStateDeadline = false;
        return Brain1::search(s, tomorrow, score, cfg);
    };
    const auto result = searchFutureDays(s, ms, roots, 3, 2,
        Clock::now() + std::chrono::seconds(5), search);
    assert(result.complete && result.selected == 1 && result.expanded == calls);
    assert(calls >= roots.size() * 2 * 2 * 3);
    const auto expired = searchFutureDays(s, ms, roots, 3, 2, Clock::now(), search);
    assert(!expired.complete && expired.expanded == 0);
    const auto failed = searchFutureDays(s, ms, roots, 3, 2,
        Clock::now() + std::chrono::seconds(1),
        [](const MatchState&, ScoreState, int, Clock::time_point) { return BeamSearchResult{}; });
    assert(!failed.complete);
    roots.push_back(roots.front());
    retainDayCandidates(roots, 3, s, ms);
    assert(roots.size() == 2);
    BeamSearchConfig cfg;
    cfg.timeLimitMs = 6000;
    cfg.quantityUpgrades = true;
    cfg.safetyMarginMs = 0;
    cfg.beamWidth = 8;
    cfg.jointBeamWidth = 16;
    cfg.autoSizedBeam = true;
    cfg.threadCount = 1;
    const auto integrated = brain1(s, ms, {}, cfg);
    assert(integrated.success);
    assert(integrated.udonAlnsAttempts > 0 && integrated.dayBeamCandidates > 0);
    const auto replay = simulateDay(s, ms, integrated.plan);
    assert(replay.valid && replay.state == integrated.finalState);
    assert(integrated.elapsedMs < 6500);
}

void checkMovingSupplyKeepsEarlyRefuel() {
    auto s = line();
    s.field.width = 7;
    s.field.cells.assign(7, {CellType::Plain, -1});
    s.field.spots = {{0, 0, 1, -1}, {0, 2, 1, -1}, {0, 4, 1, -1}, {0, 6, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    s.daySteps = {12};
    MatchState ms;
    ms.agents = {{0, AgentKind::Patrol, 0, 2}, {1, AgentKind::Supply, 2, 2}};
    ms.trafficByPos.assign(7, TrafficStatus::Smooth);
    const DailyPlan incumbent{{2, 2, 2, 2, -4}, {-12}};
    assert(simulateDay(s, ms, incumbent).state.dayUdon == 3);
    RouteColumnConfig config;
    config.maxSupplySchedules = 8;
    const auto improved = searchRouteColumns(s, ms, {}, incumbent, {}, config,
        Clock::time_point::max());
    assert(improved.finalState.dayUdon == 4);
    assert(improved.plan[1] != incumbent[1]);
    const auto replay = simulateDay(s, ms, improved.plan);
    assert(replay.valid && replay.state == improved.finalState);
}

void checkFuelAwareOvernight() {
    auto s = line();
    s.daySteps = {8, 8};
    s.daySeconds = {60, 60};
    s.agentStarts = {0, 4};
    MatchState ms;
    ms.agents = {{0, AgentKind::Patrol, 0, 0}, {1, AgentKind::Supply, 4, 2}};
    ms.trafficByPos.assign(5, TrafficStatus::Smooth);
    DailyPlan incumbent{{-8}, {-8}};
    const auto initial = simulateDay(s, ms, incumbent);
    assert(initial.valid);
    for (int start = 0; start < 5; ++start) {
        const auto table = Brain0::dijkstra(s, ms, start);
        for (int goal = 0; goal < 5; ++goal) {
            const auto path = Brain0::restorePath(s, table, goal);
            assert(path.reachable && table.patrolFuelCosts[goal] == path.cost.fuel);
        }
    }
    std::vector<DijkstraTable> cache(5);
    std::vector<bool> cached(5, false);
    const auto before = detail::futureRoutePotential(s, ms, initial.state, cache, cached);
    assert(before.strandedPatrols == 1);
    const auto improved = improveOvernightPosition(s, ms, {}, incumbent,
        Clock::now() + std::chrono::seconds(2));
    assert(improved.improvements > 0);
    assert(improved.state.dayBrands == initial.state.dayBrands);
    assert(improved.state.dayUdon >= initial.state.dayUdon);
    assert(improved.state.agents[0].fuel == s.fuelLimit);
    assert(improved.state.agents[1].pos == 0);
    const auto checked = simulateDay(s, ms, improved.plan);
    assert(checked.valid && checked.state == improved.state);
    const auto after = detail::futureRoutePotential(s, ms, checked.state, cache, cached);
    assert(after.strandedPatrols == 0);
    const auto expired = improveOvernightPosition(s, ms, {}, incumbent, Clock::now());
    assert(expired.attempts == 0 && expired.plan == incumbent);
    const auto bounded = improveOvernightPosition(s, ms, {}, incumbent,
        Clock::now() + std::chrono::seconds(2), 1);
    assert(bounded.attempts <= 1);
    ms.day = 1;
    const auto final = improveOvernightPosition(s, ms, {}, incumbent, Clock::time_point::max());
    assert(final.attempts == 0 && final.plan == incumbent);
}

void checkOvernightReplayProperties() {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        CreateConfig config;
        config.width = config.height = 8;
        config.agentCount = 4;
        config.spotCount = 8;
        config.brandCount = 5;
        config.dayCount = 4;
        config.seed = seed;
        const auto s = ProblemCreator::generate(config).setting;
        auto ms = makeInitialState(s, {AgentKind::Patrol, AgentKind::Patrol,
                                      AgentKind::Patrol, AgentKind::Supply});
        for (std::size_t pos = 0; pos < s.field.cellCount(); ++pos)
            if (s.field.cells[pos].type == CellType::Road)
                ms.trafficByPos[pos] = static_cast<TrafficStatus>((pos + seed) % 3);
        const auto incumbent = brain0Greedy(s, ms);
        const auto initial = simulateDay(s, ms, incumbent);
        assert(initial.valid);
        const auto improved = improveOvernightPosition(s, ms, {}, incumbent,
            Clock::now() + std::chrono::milliseconds(100), 128);
        const auto replay = simulateDay(s, ms, improved.plan);
        assert(replay.valid && replay.state == improved.state);
        assert(improved.state.dayUdon >= initial.state.dayUdon);
        assert((improved.state.dayBrands & initial.state.dayBrands) == initial.state.dayBrands);
        assert(improved.attempts <= 128);
    }
}

void checkFirstRoundJointProfile() {
    MatchSetting s;
    s.field.width = s.field.height = 16;
    BeamSearchConfig cfg;
    for (const auto budget : {std::int64_t{0}, std::int64_t{1000},
                             std::int64_t{45000}}) {
        const auto profile = detail::adaptiveLongSearchProfile(
            s, cfg, budget, false, false);
        assert(profile.udonAlns.maxPatrols == 3);
        cfg.smallBoardJointRefinement = false;
        assert(detail::adaptiveLongSearchProfile(s, cfg, budget, false, false)
            .udonAlns.maxPatrols == 2);
        cfg.smallBoardJointRefinement = true;
    }
    cfg.adaptiveLongSearch = false;
    assert(detail::adaptiveLongSearchProfile(s, cfg, 1000, false, true)
        .udonAlns.maxPatrols == 3);
    s.field.height = 24;
    assert(detail::adaptiveLongSearchProfile(s, cfg, 1000, false, false)
        .udonAlns.maxPatrols == 2);
    s.field.width = 24;
    assert(detail::adaptiveLongSearchProfile(s, cfg, 1000, false, false)
        .udonAlns.maxPatrols == 2);
}

void checkFirstRoundFinalColumns() {
    auto s = line();
    s.field.width = s.field.height = 16;
    s.field.cells.assign(256, {CellType::Plain, -1});
    assert(s.field.rebuildSpotIndex());
    s.fuelLimit = 30;
    s.agentStarts = {0, 1, 2, 4};
    MatchState ms;
    ms.agents = {{0, AgentKind::Patrol, 0, 30},
                 {1, AgentKind::Patrol, 1, 30},
                 {2, AgentKind::Patrol, 2, 30},
                 {3, AgentKind::Supply, 4, 30}};
    ms.trafficByPos.assign(256, TrafficStatus::Smooth);
    BeamSearchConfig cfg;
    cfg.beamWidth = 8;
    cfg.jointBeamWidth = 32;
    cfg.threadCount = 1;
    cfg.timeLimitMs = 0;
    cfg.useStateDeadline = false;
    cfg.iterativeDeepening = false;
    cfg.adaptiveBeamWidth = false;
    cfg.useUdonAlns = false;
    cfg.coordinatedRefinement = false;
    cfg.useDayBeam = false;
    cfg.smallBoardFinalColumns = false;
    const auto before = brain1(s, ms, {}, cfg);
    cfg.smallBoardFinalColumns = true;
    const auto after = brain1(s, ms, {}, cfg);
    assert(before.success && after.success);
    assert(after.finalState.score.totalUdon >= before.finalState.score.totalUdon);
    assert(after.finalState.dayBrands == before.finalState.dayBrands);
    assert(after.routeColumnsGenerated > before.routeColumnsGenerated);
    const auto replay = simulateDay(s, ms, after.plan);
    assert(replay.valid && replay.state == after.finalState);
    s.daySteps.push_back(8);
    s.daySeconds.push_back(60);
    cfg.smallBoardFinalColumns = false;
    const auto middleBefore = brain1(s, ms, {}, cfg);
    cfg.smallBoardFinalColumns = true;
    const auto middleAfter = brain1(s, ms, {}, cfg);
    assert(middleBefore.plan == middleAfter.plan);
    assert(middleBefore.finalState == middleAfter.finalState);
    assert(middleBefore.routeColumnsGenerated == middleAfter.routeColumnsGenerated);
    s.field.width = 8;
    s.field.height = 32;
    assert(s.field.rebuildSpotIndex());
    s.daySteps.resize(1);
    s.daySeconds.resize(1);
    cfg.smallBoardFinalColumns = false;
    const auto rectBefore = brain1(s, ms, {}, cfg);
    cfg.smallBoardFinalColumns = true;
    const auto rectAfter = brain1(s, ms, {}, cfg);
    assert(rectBefore.plan == rectAfter.plan);
    assert(rectBefore.routeColumnsGenerated == rectAfter.routeColumnsGenerated);
}

int main() {
    checkFirstRoundJointProfile();
    checkFirstRoundFinalColumns();
    checkTransitAndTimedRefuel();
    checkDayBeam();
    checkMovingSupplyKeepsEarlyRefuel();
    checkFuelAwareOvernight();
    checkOvernightReplayProperties();
    std::cout << "quantity search tests passed\n";
}
