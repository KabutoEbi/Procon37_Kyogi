#include "model.hpp"

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

void checkJointRouteSearch() {
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

}  // namespace

int main() {
    checkFinalDayInsertion();
    checkIntermediateFuelDominance();
    checkJointRouteSearch();
    checkIntermediatePositionChange();
}
