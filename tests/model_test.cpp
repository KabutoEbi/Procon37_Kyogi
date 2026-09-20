#include "model.hpp"

#include <cassert>
#include <cstddef>
#include <unordered_set>

namespace {

using namespace procon37;

MatchSetting makeSetting() {
    MatchSetting setting;
    setting.daySteps = {3};
    setting.daySeconds = {60};
    setting.fuelLimit = 1;
    setting.playerCount = 2;
    setting.field.width = 3;
    setting.field.height = 3;
    setting.field.cells.assign(9, Cell{CellType::Plain, -1});
    setting.field.spots = {Spot{7, 5, 1, -1}};
    setting.agentStarts = {4, 4};
    assert(setting.field.rebuildSpotIndex());
    return setting;
}

MatchState makeState(AgentKind secondKind = AgentKind::Supply) {
    MatchState state;
    state.day = 0;
    state.agents = {
        Agent{0, AgentKind::Patrol, 4, 1},
        Agent{1, secondKind, 4, 1},
    };
    state.trafficByPos.assign(9, TrafficStatus::Smooth);
    return state;
}

void testHexNeighbors() {
    Field field;
    field.width = 3;
    field.height = 3;
    field.cells.assign(9, Cell{CellType::Plain, -1});

    // 偶数行 (x=1,y=0): 左下は (1,1)、右下は (2,1)。
    assert(field.neighbor(1, Direction::LowerLeft) == 4);
    assert(field.neighbor(1, Direction::LowerRight) == 5);

    // 奇数行 (x=1,y=1): 左上は (0,0)、右上は (1,0)。
    assert(field.neighbor(4, Direction::UpperLeft) == 0);
    assert(field.neighbor(4, Direction::UpperRight) == 1);
}

void testMoveAcquireAndRefuel() {
    const MatchSetting setting = makeSetting();
    const MatchState matchState = makeState();
    const DailyPlan plan = {{2, -1}, {2, -1}};

    const SimulationResult result =
        simulateDay(setting, matchState, plan);
    assert(result.valid);
    assert(result.state.agents[0].pos == 5);
    assert(result.state.agents[0].fuel == setting.fuelLimit);
    assert(result.state.dayUdon == 1);
    assert(result.state.remainingStocks[0] == 0);
    assert(countBits(result.state.dayBrands) == 1);
    assert(result.state.score.sumOfDailyBrandCounts == 1);
}

void testAcquisitionOrder() {
    MatchSetting setting = makeSetting();
    setting.daySteps = {1};

    MatchState state = makeState(AgentKind::Patrol);
    state.agents[0].pos = 5;
    state.agents[1].pos = 5;
    const DailyPlan plan = {{-1}, {-1}};

    const SimulationResult result = simulateDay(setting, state, plan);
    assert(result.valid);
    assert(result.state.hasAcquired(0, 0));
    assert(!result.state.hasAcquired(1, 0));
}

void testFuelValidation() {
    const MatchSetting setting = makeSetting();
    MatchState state = makeState();
    state.agents[0].fuel = 0;

    // step 0 ではまだ補給フェーズを経ていないため、即移動は不正。
    const SimulationResult invalid =
        simulateDay(setting, state, {{2, -1}, {2, -1}});
    assert(!invalid.valid);
    assert(invalid.failure.error == SimulationError::InsufficientFuel);

    // 1ステップ待機して補給を受ければ、その後の移動は可能。
    const SimulationResult valid =
        simulateDay(setting, state, {{-1, 2}, {-1, 2}});
    assert(valid.valid);
}

void testHashAndEvaluation() {
    const MatchSetting setting = makeSetting();
    const MatchState state = makeState();
    const SimulationResult result =
        simulateDay(setting, state, {{2, -1}, {2, -1}});
    assert(result.valid);

    std::unordered_set<GameState, GameStateHash, GameStateEqual> states;
    states.insert(result.state);
    assert(states.count(result.state) == 1);

    GameState changed = result.state;
    --changed.agents[0].fuel;
    assert(states.count(changed) == 0);

    EvaluationContext context;
    const EvaluationBreakdown evaluation =
        evaluateGameState(result.state, context);
    assert(evaluation.newBrandCount == 1);
    assert(evaluation.dailyBrandCount == 1);
    assert(evaluation.udonCount == 1);
}

void testDijkstra() {
    MatchSetting setting = makeSetting();
    setting.field.cells[5].type = CellType::Road;

    MatchState state = makeState();
    state.trafficByPos[5] = TrafficStatus::Smooth;

    // 4(平地) -> 5(道路) -> 2。移動元コストなので 2 + 1 = 3。
    const DijkstraTable table = Brain0::dijkstra(setting, state, 4);
    assert(table.canReach(2));
    assert(table.distances[2] == 3);

    const PathResult patrol =
        Brain0::restorePath(setting, table, 2, AgentKind::Patrol);
    assert(patrol.reachable);
    assert(patrol.cost.steps == 3);
    assert(patrol.cost.fuel == 3);
    assert((patrol.positions == std::vector<CellPos>{4, 5, 2}));
    assert((patrol.directions ==
            std::vector<Direction>{
                Direction::Right,
                Direction::UpperRight,
            }));

    const PathResult supply =
        Brain0::restorePath(setting, table, 2, AgentKind::Supply);
    assert(supply.reachable);
    assert(supply.cost.fuel == 0);

    // 直接経路を池で塞ぐと、4 -> 1 -> 2 の迂回路になる。
    setting.field.cells[5].type = CellType::Pond;
    const PathResult detour =
        Brain0::shortestPath(setting, state, 4, 2);
    assert(detour.reachable);
    assert(detour.cost.steps == 4);
    assert((detour.positions == std::vector<CellPos>{4, 1, 2}));

    const PathResult invalid =
        Brain0::shortestPath(setting, state, kInvalidCell, 2);
    assert(!invalid.reachable);
}

}  // namespace

int main() {
    testHexNeighbors();
    testMoveAcquireAndRefuel();
    testAcquisitionOrder();
    testFuelValidation();
    testHashAndEvaluation();
    testDijkstra();
}
