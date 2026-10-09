#include "model.hpp"
#include "route_column_search.hpp"

#include <cassert>
#include <chrono>

using namespace benzen;

namespace {

void checkFinalDayInsertion() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells.assign(4, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {0, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 8;

    MatchState state;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.agents.push_back({0, AgentKind::Patrol, 0, 8});
    const DailyPlan incumbent{{2, -6}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 1);

    SpotRouteLnsConfig config;
    config.maxAttempts = 64;
    config.maxInsertions = 2;
    const auto result = improveSpotRoutes(setting, state, {}, incumbent, config,
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements == 1);
    assert(result.finalState.dayUdon == 2);
    assert((result.finalState.dayBrands & before.state.dayBrands) ==
           before.state.dayBrands);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkTerminalWaitFillsSeveralSpots() {
    MatchSetting setting;
    setting.field.width = 5;
    setting.field.height = 1;
    setting.field.cells.assign(5, {CellType::Plain, -1});
    setting.field.spots = {
        {0, 1, 1, -1}, {0, 2, 1, -1}, {0, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 8;

    MatchState state;
    state.trafficByPos.assign(5, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 8}};
    const DailyPlan incumbent{{2, -6}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 1);

    SpotRouteLnsConfig config;
    config.terminalWaitFill = true;
    config.terminalFillRounds = 1;
    config.terminalFillCandidates = 1;
    const auto result = fillTerminalPatrolWaits(
        setting, state, {}, incumbent, config,
        std::chrono::steady_clock::time_point::max());
    // Even with one candidate and one round, prefer the path to spot 3 because
    // it passes through spot 2 and therefore gains two bowls instead of one.
    assert(result.improvements >= 1);
    assert(result.finalState.dayUdon == 3);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRoutePathCountsTransitPickups() {
    MatchSetting setting;
    setting.field.width = 5;
    setting.field.height = 1;
    setting.field.cells.assign(5, {CellType::Plain, -1});
    setting.field.spots = {
        {0, 1, 1, -1}, {1, 2, 1, -1}, {2, 4, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 8;
    MatchState state;
    state.trafficByPos.assign(5, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 8}};
    const auto path = Brain0::shortestPath(
        setting, state, 0, 4, AgentKind::Patrol);
    assert(path.reachable);
    const auto spots = detail::routePathSpots(setting, path);
    assert(countBits(spots) == 3);
    assert((spots & spotBit(0)) != 0);
    assert((spots & spotBit(1)) != 0);
    assert((spots & spotBit(2)) != 0);
}

void checkJointRouteSearch() {
    assert(!SpotRouteLnsConfig{}.spotPortfolioReachabilityRarity);
    MatchSetting setting;
    setting.field.width = 8;
    setting.field.height = 1;
    setting.field.cells.assign(8, {CellType::Plain, -1});
    setting.field.spots = {
        {0, 1, 2, -1}, {1, 3, 2, -1},
        {1, 4, 2, -1}, {0, 6, 2, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 7};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 8;
    MatchState state;
    state.trafficByPos.assign(8, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 8},
                    {1, AgentKind::Patrol, 7, 8}};
    const DailyPlan incumbent{{2, -6}, {5, -6}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 2);
    SpotRouteLnsConfig config;
    config.maxTeamAttempts = 200;
    const auto result = improveTeamSpotRoutes(setting, state, {}, incumbent,
        config, std::chrono::steady_clock::time_point::max());
    assert(result.improvements > 0);
    assert(result.teamAttempts > 0);
    assert(result.finalState.dayUdon > before.state.dayUdon);
    assert((result.finalState.dayBrands & before.state.dayBrands) ==
           before.state.dayBrands);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);

    const auto portfolio = searchSpotPortfolio(setting, state, {}, incumbent,
        config, std::chrono::steady_clock::time_point::max());
    assert(portfolio.portfolioMaxBrands >= 2);
    assert(portfolio.finalState.dayUdon > before.state.dayUdon);
    const auto portfolioReplay = simulateDay(setting, state, portfolio.plan);
    assert(portfolioReplay.valid && portfolioReplay.state == portfolio.finalState);
}

void checkPortfolioPrefersNewMatchBrand() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells.assign(4, {CellType::Plain, -1});
    setting.field.spots = {
        {0, 1, 1, -1},
        {1, 3, 1, -1},
    };
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 4;

    MatchState state;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 4}};
    ScoreState previous;
    previous.matchBrands = brandBit(0);

    SpotRouteLnsConfig config;
    config.spotPortfolioBeamWidth = 1;
    config.spotPortfolioCandidatesPerDepth = 1;
    config.spotPortfolioHardnessWeight = 0;
    const DailyPlan incumbent{{-8}};
    const auto result = searchSpotPortfolio(
        setting, state, previous, incumbent, config,
        std::chrono::steady_clock::time_point::max());

    assert(result.improvements == 1);
    assert((result.finalState.score.matchBrands & brandBit(1)) != 0);
    assert(countBits(result.finalState.score.matchBrands) == 2);
    assert(result.finalState.score.totalUdon == 2);
    const auto replay = simulateDay(setting, state, result.plan, previous);
    assert(replay.valid && replay.state == result.finalState);
}

void checkIntermediatePositionChange() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells.assign(4, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {0, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {8, 8};
    setting.daySeconds = {60, 60};
    setting.fuelLimit = 8;
    MatchState state;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.agents.push_back({0, AgentKind::Patrol, 0, 8});
    const DailyPlan incumbent{{2, -6}};
    const auto before = simulateDay(setting, state, incumbent);
    SpotRouteLnsConfig config;
    config.maxAttempts = 64;
    const auto result = improveSpotRoutes(setting, state, {}, incumbent,
        config, std::chrono::steady_clock::time_point::max());
    assert(result.finalState.dayUdon == 2);
    assert(result.finalState.agents[0].pos != before.state.agents[0].pos);
    assert(result.finalState.dayBrands == before.state.dayBrands);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkIntermediateFuelDominance() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells.assign(4, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {0, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {16, 16};
    setting.daySeconds = {60, 60};
    setting.fuelLimit = 16;

    MatchState state;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.agents.push_back({0, AgentKind::Patrol, 0, 16});
    const DailyPlan incumbent{{2, 2, 5, 2, 5, 5, 2, 5}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 1);

    SpotRouteLnsConfig config;
    config.maxAttempts = 64;
    config.maxInsertions = 2;
    config.allowFuturePositionChange = false;
    const auto result = improveSpotRoutes(setting, state, {}, incumbent, config,
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements == 1);
    assert(result.finalState.dayUdon == 2);
    assert(result.finalState.agents[0].pos == before.state.agents[0].pos);
    assert(result.finalState.agents[0].fuel > before.state.agents[0].fuel);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsWithRefuel() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells.assign(4, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {0, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 1};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 2;
    MatchState state;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 1},
                    {1, AgentKind::Supply, 1, 0}};
    const DailyPlan incumbent{{2, -6}, {-8}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 1);
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, RouteColumnConfig{},
        std::chrono::steady_clock::time_point::max());
    assert(result.generated > 0 && result.masterNodes > 0);
    assert(result.improvements > 0 && result.finalState.dayUdon == 2);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsRespectStock() {
    MatchSetting setting;
    setting.field.width = 5;
    setting.field.height = 1;
    setting.field.cells.assign(5, {CellType::Plain, -1});
    setting.field.spots = {{0, 2, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 4};
    setting.daySteps = {6};
    setting.daySeconds = {60};
    setting.fuelLimit = 5;
    MatchState state;
    state.trafficByPos.assign(5, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 5},
                    {1, AgentKind::Patrol, 4, 5}};
    const DailyPlan incumbent{{-6}, {-6}};
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, RouteColumnConfig{},
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements > 0);
    assert(result.finalState.dayUdon == 1);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsCombinePatrols() {
    MatchSetting setting;
    setting.field.width = 5;
    setting.field.height = 1;
    setting.field.cells.assign(5, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {1, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 4};
    setting.daySteps = {2};
    setting.daySeconds = {60};
    setting.fuelLimit = 2;
    MatchState state;
    state.trafficByPos.assign(5, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 2},
                    {1, AgentKind::Patrol, 4, 2}};
    const DailyPlan incumbent{{-2}, {-2}};
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, RouteColumnConfig{},
        std::chrono::steady_clock::time_point::max());
    assert(result.finalState.dayUdon == 2);
    assert(countBits(result.finalState.dayBrands) == 2);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsSupplyStation() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells.assign(4, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {0, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 2};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 2;
    MatchState state;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 1},
                    {1, AgentKind::Supply, 2, 0}};
    const DailyPlan incumbent{{2, -6}, {-8}};
    assert(simulateDay(setting, state, incumbent).state.dayUdon == 1);
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, RouteColumnConfig{},
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements > 0 && result.finalState.dayUdon == 2);
    assert(result.plan[1] != incumbent[1]);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsUseEverySupplyForStations() {
    MatchSetting setting;
    setting.field.width = 7;
    setting.field.height = 1;
    setting.field.cells.assign(7, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {1, 2, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 6, 2};
    setting.daySteps = {4};
    setting.daySeconds = {60};
    setting.fuelLimit = 1;
    MatchState state;
    state.trafficByPos.assign(7, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 1},
                    {1, AgentKind::Supply, 6, 0},
                    {2, AgentKind::Supply, 2, 0}};
    const DailyPlan incumbent{{2, -2}, {-4}, {-4}};
    assert(simulateDay(setting, state, incumbent).state.dayUdon == 1);
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, RouteColumnConfig{},
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements > 0 && result.finalState.dayUdon == 2);
    assert(result.plan[1] == incumbent[1]);
    assert(result.plan[2] != incumbent[2]);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsLargePool() {
    MatchSetting setting;
    setting.field.width = 14;
    setting.field.height = 1;
    setting.field.cells.assign(14, {CellType::Plain, -1});
    for (int pos = 1; pos <= 12; ++pos)
        setting.field.spots.push_back({pos % 3, pos, 2, -1});
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 13};
    setting.daySteps = {24};
    setting.daySeconds = {60};
    setting.fuelLimit = 24;
    MatchState state;
    state.trafficByPos.assign(14, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 24},
                    {1, AgentKind::Patrol, 13, 24}};
    const DailyPlan incumbent{{-24}, {-24}};
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, RouteColumnConfig{},
        std::chrono::steady_clock::time_point::max());
    assert(result.generated > 100);
    assert(result.masterNodes > 0);
    assert(countBits(result.finalState.dayBrands) == 3);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsKeepSearchSlotBesideIncumbent() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells.assign(4, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}, {1, 3, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 8;

    MatchState state;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 8}};
    const DailyPlan incumbent{{-8}};
    RouteColumnConfig config;
    config.columnsPerAgent = 1;
    config.supplyRendezvous = false;
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, config,
        std::chrono::steady_clock::time_point::max());

    assert(result.improvements > 0);
    assert(countBits(result.finalState.dayBrands) == 2);
    assert(result.finalState.dayUdon == 2);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkQuantityFirstPortfolioRequiresAllBrands() {
    MatchSetting setting;
    setting.field.width = 7;
    setting.field.height = 1;
    setting.field.cells.assign(7, {CellType::Plain, -1});
    setting.field.spots = {
        {0, 0, 1, -1}, {0, 1, 1, -1}, {0, 2, 1, -1},
        {1, 6, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {4};
    setting.daySteps = {14};
    setting.daySeconds = {60};
    setting.fuelLimit = 8;
    MatchState state;
    state.trafficByPos.assign(7, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 4, 8}};
    const DailyPlan incumbent{{-14}};
    const auto feasible = simulateDay(setting, state,
        DailyPlan{{5, 5, 2, 2, 2, 2, -2}});
    assert(feasible.valid && countBits(feasible.state.dayBrands) == 2);

    SpotRouteLnsConfig config;
    config.quantityFirstRanking = true;
    config.requiredMatchBrandCount = 2;
    config.requiredDayBrandCount = 2;
    config.reflectTransit = false;
    config.spotPortfolioBeamWidth = 128;
    config.spotPortfolioCandidatesPerDepth = 128;
    const auto result = searchSpotPortfolio(setting, state, {}, incumbent,
        config, std::chrono::steady_clock::time_point::max());

    assert(result.portfolioReachedRequiredBrands);
    assert(countBits(result.finalState.score.matchBrands) == 2);
    assert(countBits(result.finalState.dayBrands) == 2);
    assert(result.finalState.dayUdon >= 2);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsRepairBrandsAfterQuantityRoute() {
    MatchSetting setting;
    setting.field.width = 3;
    setting.field.height = 3;
    setting.field.cells.assign(9, {CellType::Plain, -1});
    setting.field.spots = {
        {0, 0, 1, -1}, {0, 1, 1, -1},
        {0, 2, 1, -1}, {1, 7, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {4};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 8;
    MatchState state;
    state.trafficByPos.assign(9, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 4, 8}};
    const DailyPlan incumbent{{-8}};
    const DailyPlan quantityOnly{{1, 2, 5, 5}};
    const auto quantityReplay = simulateDay(setting, state, quantityOnly);
    assert(quantityReplay.valid);
    assert(quantityReplay.state.dayUdon == 3);
    assert(countBits(quantityReplay.state.dayBrands) == 1);
    RouteColumnConfig config;
    config.columnsPerAgent = 1;
    config.supplyRendezvous = false;
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, config,
        std::chrono::steady_clock::time_point::max());
    assert(result.generated > 0);
    assert(countBits(result.finalState.dayBrands) == 2);
    assert(result.finalState.dayUdon >= 3);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsSuffixInsertionWithoutTerminalWait() {
    MatchSetting setting;
    setting.field.width = 3;
    setting.field.height = 2;
    setting.field.cells.assign(6, {CellType::Plain, -1});
    setting.field.spots = {
        {0, 1, 1, -1}, {1, 4, 1, -1}, {2, 5, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0};
    setting.daySteps = {6};
    setting.daySeconds = {60};
    setting.fuelLimit = 10;
    MatchState state;
    state.trafficByPos.assign(6, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 10}};
    const DailyPlan incumbent{{2, 4, 1}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 2);
    RouteColumnConfig config;
    config.maxVisits = 0; // Exercise incumbent-derived suffixes only.
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, config,
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements > 0);
    assert(result.finalState.dayUdon == 3);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsRescueStrandedPatrol() {
    MatchSetting setting;
    setting.field.width = 3;
    setting.field.height = 1;
    setting.field.cells.assign(3, {CellType::Plain, -1});
    setting.field.spots = {{0, 1, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 2};
    setting.daySteps = {8};
    setting.daySeconds = {60};
    setting.fuelLimit = 4;
    MatchState state;
    state.trafficByPos.assign(3, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 0},
                    {1, AgentKind::Supply, 2, 4}};
    const DailyPlan incumbent{{-8}, {-8}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 0);

    RouteColumnConfig config;
    config.maxStationSchedules = 0;
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, config,
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements > 0);
    assert(result.finalState.dayUdon == 1);
    assert(result.finalState.agents[0].fuel > 0);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

void checkRouteColumnsKeepEqualScoreRescue() {
    MatchSetting setting;
    setting.field.width = 3;
    setting.field.height = 1;
    setting.field.cells.assign(3, {CellType::Plain, -1});
    setting.field.spots = {{0, 0, 1, -1}, {1, 2, 1, -1}};
    assert(setting.field.rebuildSpotIndex());
    setting.agentStarts = {0, 2};
    setting.daySteps = {6, 6};
    setting.daySeconds = {60, 60};
    setting.fuelLimit = 4;
    MatchState state;
    state.day = 0;
    state.trafficByPos.assign(3, TrafficStatus::Smooth);
    state.agents = {{0, AgentKind::Patrol, 0, 0},
                    {1, AgentKind::Supply, 2, 4}};
    const DailyPlan incumbent{{-6}, {-6}};
    const auto before = simulateDay(setting, state, incumbent);
    assert(before.valid && before.state.dayUdon == 1);

    RouteColumnConfig config;
    config.maxStationSchedules = 0;
    const auto result = searchRouteColumns(setting, state, {}, incumbent,
        SpotRouteLnsConfig{}, config,
        std::chrono::steady_clock::time_point::max());
    assert(result.improvements > 0);
    assert(result.finalState.dayUdon == before.state.dayUdon);
    assert(result.finalState.agents[0].fuel > before.state.agents[0].fuel);
    assert(result.plan[1] != incumbent[1]);
    const auto replay = simulateDay(setting, state, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

}  // namespace

int main() {
    checkFinalDayInsertion();
    checkTerminalWaitFillsSeveralSpots();
    checkRoutePathCountsTransitPickups();
    checkIntermediateFuelDominance();
    checkJointRouteSearch();
    checkPortfolioPrefersNewMatchBrand();
    checkIntermediatePositionChange();
    checkRouteColumnsWithRefuel();
    checkRouteColumnsRespectStock();
    checkRouteColumnsCombinePatrols();
    checkRouteColumnsSupplyStation();
    checkRouteColumnsUseEverySupplyForStations();
    checkRouteColumnsLargePool();
    checkRouteColumnsKeepSearchSlotBesideIncumbent();
    checkQuantityFirstPortfolioRequiresAllBrands();
    checkRouteColumnsRepairBrandsAfterQuantityRoute();
    checkRouteColumnsSuffixInsertionWithoutTerminalWait();
    checkRouteColumnsRescueStrandedPatrol();
    checkRouteColumnsKeepEqualScoreRescue();
}
