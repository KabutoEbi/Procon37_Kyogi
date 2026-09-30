#include "application.hpp"
#include "model.hpp"

#include <algorithm>
#include <cassert>
#include <set>

using namespace benzen;

namespace {

MatchSetting officialLine() {
    MatchSetting setting;
    setting.field.width = 4;
    setting.field.height = 1;
    setting.field.cells = {
        {CellType::Road, -1},
        {CellType::Road, -1},
        {CellType::Plain, -1},
        {CellType::Plain, -1},
    };
    setting.field.spots = {{10, 2, 4, -1}};
    setting.agentStarts = {0, 0, 0, 0, 2, 3, 3, 2};
    setting.daySteps = {6};
    setting.daySeconds = {60};
    setting.fuelLimit = 3;
    setting.playerCount = 2;
    setting.busyThreshold = 1;
    setting.jammedThreshold = 2;
    assert(setting.field.rebuildSpotIndex());
    return setting;
}

void checkQ6Supplement() {
    const MatchSetting setting = officialLine();
    MatchState state;
    state.day = 0;
    state.trafficByPos.assign(4, TrafficStatus::Smooth);
    state.trafficByPos[1] = TrafficStatus::Busy;
    for (AgentIndex a = 0; a < setting.agentCount(); ++a) {
        state.agents.push_back({
            a,
            a == 7 ? AgentKind::Supply : AgentKind::Patrol,
            setting.agentStarts[a],
            setting.fuelLimit,
        });
    }
    const int right = static_cast<int>(Direction::Right);
    const int left = static_cast<int>(Direction::Left);
    const DailyPlan plan{
        {right, right, right, -1}, {-1, right, right, right}, {-2, right, right, -1},
        {-3, right, -2},           {left, left, -2},          {left, left, -2},
        {-2, left, left},          {left, left, -2},
    };
    const auto result = simulateDay(setting, state, plan);
    assert(result.valid);
    assert(result.state.dayUdon == 4);
    for (const AgentIndex acquired : {AgentIndex{0}, AgentIndex{1}, AgentIndex{4}, AgentIndex{5}}) {
        assert(result.state.hasAcquired(acquired, 0));
    }
    assert(!result.state.hasAcquired(2, 0));
    assert(!result.state.hasAcquired(6, 0));
    assert(result.state.agents[4].fuel == setting.fuelLimit);
    assert(result.roadStaySteps[0] == 12);
    assert(result.roadStaySteps[1] == 17);
}

MatchSetting line(int width, int steps, int fuel) {
    MatchSetting setting;
    setting.field.width = width;
    setting.field.height = 1;
    setting.field.cells.assign(width, {CellType::Plain, -1});
    setting.agentStarts = {0, width - 1};
    setting.daySteps = {steps};
    setting.daySeconds = {60};
    setting.fuelLimit = fuel;
    setting.playerCount = 2;
    assert(setting.field.rebuildSpotIndex());
    return setting;
}

MatchState twoAgents(const MatchSetting& setting, CellPos patrol, CellPos supply) {
    MatchState state;
    state.day = 0;
    state.trafficByPos.assign(setting.field.cellCount(), TrafficStatus::Smooth);
    state.agents = {
        {0, AgentKind::Patrol, patrol, setting.fuelLimit},
        {1, AgentKind::Supply, supply, setting.fuelLimit},
    };
    return state;
}

void checkPhaseOrderAndMovementRules() {
    const int right = static_cast<int>(Direction::Right);
    const int left = static_cast<int>(Direction::Left);

    // Q22: 同時にセルを入れ替えるだけなら補給されない。
    auto setting = line(2, 2, 2);
    auto state = twoAgents(setting, 0, 1);
    auto result = simulateDay(setting, state, {{right}, {left}});
    assert(result.valid && result.state.agents[0].fuel == 1);

    // Q22/Q48: 最終stepで合流しても補給され、同行中は満タンに戻る。
    setting = line(3, 4, 1);
    state = twoAgents(setting, 0, 0);
    result = simulateDay(setting, state, {{right, right}, {right, right}});
    assert(result.valid);
    assert(result.state.agents[0].pos == 2);
    assert(result.state.agents[0].fuel == setting.fuelLimit);

    // Q25/Q49: 移動元地形のコストを使い、待機は地形に依存しない。
    setting = line(2, 3, 2);
    setting.field.cells[0].type = CellType::Mountain;
    state = twoAgents(setting, 0, 1);
    state.agents[1].kind = AgentKind::Patrol;
    result = simulateDay(setting, state, {{right}, {-3}});
    assert(result.valid && result.state.agents[0].fuel == 0);
}

void checkAcquisitionAndTrafficOrder() {
    auto setting = line(2, 2, 3);
    setting.field.spots = {{7, 0, 1, -1}};
    setting.agentStarts = {0, 0};
    assert(setting.field.rebuildSpotIndex());
    MatchState state;
    state.day = 0;
    state.trafficByPos.assign(2, TrafficStatus::Smooth);
    state.agents = {
        {0, AgentKind::Patrol, 0, 3},
        {1, AgentKind::Patrol, 0, 3},
    };
    const auto stock = simulateDay(setting, state, {{-2}, {-2}});
    assert(stock.valid && stock.state.dayUdon == 1);
    assert(stock.state.hasAcquired(0, 0));
    assert(!stock.state.hasAcquired(1, 0));

    setting = line(2, 2, 3);
    setting.field.cells[0].type = CellType::Road;
    setting.field.cells[1].type = CellType::Road;
    state = twoAgents(setting, 0, 0);
    const int right = static_cast<int>(Direction::Right);
    const auto traffic = simulateDay(setting, state, {{right, -1}, {-2}});
    assert(traffic.valid);
    assert(traffic.roadStaySteps[0] == 2);
    assert(traffic.roadStaySteps[1] == 2);
}

void checkGeometryAndPublishedRanges() {
    Field field;
    field.width = field.height = 3;
    field.cells.assign(9, {CellType::Plain, -1});
    assert(field.neighbor(1, Direction::LowerLeft) == 4);
    assert(field.neighbor(1, Direction::LowerRight) == 5);
    assert(field.neighbor(4, Direction::UpperLeft) == 0);
    assert(field.neighbor(4, Direction::UpperRight) == 1);
    assert(field.neighbor(0, Direction::Left) == kInvalidCell);

    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        CreateConfig config;
        config.width = config.height = 16;
        config.agentCount = 4;
        config.spotCount = 16;
        config.brandCount = 12;
        config.seed = seed;
        const auto setting = ProblemCreator::generate(config).setting;
        assert(setting.busyThreshold >= 1 && setting.busyThreshold <= 5);
        assert(setting.jammedThreshold >= 2 && setting.jammedThreshold <= 10);
        assert(setting.fuelLimit >= setting.daySteps.front());
        assert(setting.fuelLimit <= 3 * setting.daySteps.front());
        assert(setting.daySteps.front() >= setting.field.width + setting.field.height);
        assert(setting.daySteps.front() <= 4 * (setting.field.width + setting.field.height));
        assert(std::set<CellPos>(setting.agentStarts.begin(), setting.agentStarts.end()).size() ==
               setting.agentStarts.size());
        for (const CellType type :
             {CellType::Plain, CellType::Road, CellType::Mountain, CellType::Pond}) {
            assert(std::any_of(setting.field.cells.begin(), setting.field.cells.end(),
                               [type](const Cell& cell) { return cell.type == type; }));
        }
    }
}

void checkNoAnswerFallback() {
    auto setting = line(3, 7, 3);
    const auto state = twoAgents(setting, 0, 2);
    const auto plan = makeWaitPlan(setting, state);
    assert(plan == DailyPlan({{-7}, {-7}}));
    assert(simulateDay(setting, state, plan).valid);
}

} // namespace

int main() {
    checkQ6Supplement();
    checkPhaseOrderAndMovementRules();
    checkAcquisitionAndTrafficOrder();
    checkGeometryAndPublishedRanges();
    checkNoAnswerFallback();
}
