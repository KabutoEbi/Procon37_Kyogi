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
}
