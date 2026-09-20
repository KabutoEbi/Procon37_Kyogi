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
    const DijkstraTable table = brain0(setting, state, 4);
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

void testBeamSearch() {
    const MatchSetting setting = makeSetting();
    const MatchState state = makeState();

    BeamSearchConfig config;
    config.beamWidth = 64;
    config.jointBeamWidth = 256;
    const BeamSearchResult result = brain1(setting, state, {}, config);

    assert(result.success);
    assert(result.plan.size() == state.agents.size());
    assert(result.finalState.finalized);
    assert(result.finalState.dayUdon == 1);
    assert(validateDailyPlan(setting, state, result.plan).valid);
}

void testAgentSelector() {
    const MatchSetting setting = makeSetting();
    AgentSelectorConfig config;
    config.trialCandidateLimit = 2;
    config.trialBeamWidth = 16;
    config.trialJointBeamWidth = 64;

    const AgentSelectionResult first = selectAgentKinds(setting, config);
    const AgentSelectionResult second = selectAgentKinds(setting, config);
    assert(first.success);
    assert(first.kinds == second.kinds);
    assert(first.kinds.size() == setting.agentCount());
    assert(first.evaluatedAssignments == 3);
    assert(first.trialAssignments == 2);
    assert(std::count(
               first.kinds.begin(),
               first.kinds.end(),
               AgentKind::Supply) == first.supplyCount);
    assert(std::count(
               first.kinds.begin(),
               first.kinds.end(),
               AgentKind::Patrol) >= 1);

    config.minSupplyCount = 1;
    config.maxSupplyCount = 1;
    config.trialCandidateLimit = 0;
    const AgentSelectionResult fixedCount =
        selectAgentKinds(setting, config);
    assert(fixedCount.success);
    assert(fixedCount.supplyCount == 1);
    assert(fixedCount.evaluatedAssignments == 2);
    assert(fixedCount.trialAssignments == 0);

}

void testProblemCreator() {
    CreateConfig config;
    config.width = 8;
    config.height = 8;
    config.agentCount = 3;
    config.spotCount = 8;
    config.brandCount = 4;
    config.dayCount = 4;
    config.teamCount = 2;
    config.seed = 123456789;

    const Field terrainOnly = ProblemCreator::generateField(config);
    const GeneratedProblem first = ProblemCreator::generate(config);
    const GeneratedProblem second = ProblemCreator::generate(config);
    const std::string json = ProblemCreator::toJson(first);

    assert(json == ProblemCreator::toJson(second));
    assert(first.setting.field.cells.size() == 64);
    assert(terrainOnly.cells.size() == 64);
    assert(terrainOnly.spots.empty());
    for (std::size_t i = 0; i < terrainOnly.cells.size(); ++i) {
        assert(terrainOnly.cells[i].type == first.setting.field.cells[i].type);
    }
    assert(first.setting.field.spots.size() == 8);
    assert(first.setting.field.brands.size() == 4);
    assert(first.setting.agentStarts.size() == 3);
    assert(first.setting.daySteps.size() == 4);
    assert(first.setting.daySeconds.size() == 4);
    assert(first.teams.size() == 2);
    assert(first.setting.fuelLimit >= first.setting.daySteps.front());
    assert(first.setting.fuelLimit <= first.setting.daySteps.front() * 3);
    assert(json.find("\"problem\"") != std::string::npos);
    assert(json.find("\"agentStarts\"") != std::string::npos);
    assert(json.find("\"fuelLimits\"") != std::string::npos);
    assert(json.find("\"teams\"") != std::string::npos);
    assert(ProblemCreator::writeJson(
        first, "/tmp/procon37-generated-problem-test.json"));

    MatchSetting setting = first.setting;
    setting.startsAt = 1'800'000'000;
    const std::string settingJson =
        ProblemCreator::toMatchSettingJson(setting);
    assert(settingJson.find("\"startsAt\": 1800000000") !=
           std::string::npos);
    assert(settingJson.find("\"map\"") != std::string::npos);
    assert(settingJson.find("\"spots\"") != std::string::npos);
    assert(settingJson.find("\"agents\"") != std::string::npos);
    assert(settingJson.find("\"players\": 2") != std::string::npos);
    assert(settingJson.find("\"problem\"") == std::string::npos);
    assert(createMatchSettingJson(
        setting, "/tmp/procon37-match-setting-test.json"));

    CreateMatchStateConfig stateConfig;
    stateConfig.day = 0;
    stateConfig.endsAt = 1'800'000'060;
    stateConfig.supplyCount = 1;
    stateConfig.seed = 987654321;
    const MatchState initial =
        ProblemCreator::generateMatchState(setting, stateConfig);
    assert(initial.day == 0);
    assert(initial.endsAt == stateConfig.endsAt);
    assert(initial.agents.size() == setting.agentCount());
    assert(initial.others.size() ==
           static_cast<std::size_t>(setting.playerCount - 1));
    for (std::size_t i = 0; i < initial.agents.size(); ++i) {
        assert(initial.agents[i].index == i);
        assert(initial.agents[i].pos == setting.agentStarts[i]);
        assert(initial.agents[i].fuel == setting.fuelLimit);
    }
    assert(initial.agents.back().kind == AgentKind::Supply);
    for (const TrafficStatus traffic : initial.trafficByPos) {
        assert(traffic == TrafficStatus::Smooth);
    }

    const std::string stateJson =
        ProblemCreator::toMatchStateJson(initial, setting.field);
    assert(stateJson.find("\"endsAt\": 1800000060") !=
           std::string::npos);
    assert(stateJson.find("\"day\": 0") != std::string::npos);
    assert(stateJson.find("\"others\"") != std::string::npos);
    assert(stateJson.find("\"traffics\"") != std::string::npos);
    assert(createMatchStateJson(
        initial,
        setting.field,
        "/tmp/procon37-match-state-test.json"));

    const MatchState directlyCreated = createMatchStateJson(
        setting,
        stateConfig,
        "/tmp/procon37-match-state-direct-test.json");
    assert(ProblemCreator::toMatchStateJson(
               directlyCreated, setting.field) == stateJson);

    stateConfig.day = 1;
    const MatchState later =
        ProblemCreator::generateMatchState(setting, stateConfig);
    for (const Agent& agent : later.agents) {
        assert(setting.field.contains(agent.pos));
        assert(setting.field.at(agent.pos).isPassable());
        assert(agent.fuel >= 0);
        assert(agent.fuel <= setting.fuelLimit);
    }

    std::vector<bool> occupied(64, false);
    for (const Spot& spot : first.setting.field.spots) {
        assert(first.setting.field.at(spot.pos).type == CellType::Plain);
        assert(!occupied[static_cast<std::size_t>(spot.pos)]);
        occupied[static_cast<std::size_t>(spot.pos)] = true;
    }
    for (const CellPos start : first.setting.agentStarts) {
        assert(first.setting.field.at(start).type == CellType::Plain);
        assert(!occupied[static_cast<std::size_t>(start)]);
        occupied[static_cast<std::size_t>(start)] = true;
    }
}

}  // namespace

int main() {
    testHexNeighbors();
    testMoveAcquireAndRefuel();
    testAcquisitionOrder();
    testFuelValidation();
    testHashAndEvaluation();
    testDijkstra();
    testBeamSearch();
    testAgentSelector();
    testProblemCreator();
}
