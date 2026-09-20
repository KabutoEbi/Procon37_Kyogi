#include "model.hpp"

#include <cassert>
#include <cstddef>
#include <unordered_set>

namespace {

using namespace procon37;

MatchSetting makeSetting() {
    MatchSetting s;
    s.daySteps = {3};
    s.daySeconds = {60};
    s.fuelLimit = 1;
    s.playerCount = 2;
    s.field.width = 3;
    s.field.height = 3;
    s.field.cells.assign(9, Cell{CellType::Plain, -1});
    s.field.spots = {Spot{7, 5, 1, -1}};
    s.agentStarts = {4, 4};
    assert(s.field.rebuildSpotIndex());
    return s;
}

MatchState makeState(AgentKind secondKind = AgentKind::Supply) {
    MatchState st;
    st.day = 0;
    st.agents = {
        Agent{0, AgentKind::Patrol, 4, 1},
        Agent{1, secondKind, 4, 1},
    };
    st.trafficByPos.assign(9, TrafficStatus::Smooth);
    return st;
}

void testHexNeighbors() {
    Field f;
    f.width = 3;
    f.height = 3;
    f.cells.assign(9, Cell{CellType::Plain, -1});

    // 偶数行 (x=1,y=0): 左下は (1,1)、右下は (2,1)。
    assert(f.neighbor(1, Direction::LowerLeft) == 4);
    assert(f.neighbor(1, Direction::LowerRight) == 5);

    // 奇数行 (x=1,y=1): 左上は (0,0)、右上は (1,0)。
    assert(f.neighbor(4, Direction::UpperLeft) == 0);
    assert(f.neighbor(4, Direction::UpperRight) == 1);
}

void testMoveAcquireAndRefuel() {
    const MatchSetting s = makeSetting();
    const MatchState st = makeState();
    const DailyPlan plan = {{2, -1}, {2, -1}};

    const auto res = simulateDay(s, st, plan);
    assert(res.valid);
    assert(res.state.agents[0].pos == 5);
    assert(res.state.agents[0].fuel == s.fuelLimit);
    assert(res.state.dayUdon == 1);
    assert(res.state.remainingStocks[0] == 0);
    assert(countBits(res.state.dayBrands) == 1);
    assert(res.state.score.sumOfDailyBrandCounts == 1);
}

void testAcquisitionOrder() {
    MatchSetting s = makeSetting();
    s.daySteps = {1};

    MatchState st = makeState(AgentKind::Patrol);
    st.agents[0].pos = 5;
    st.agents[1].pos = 5;
    const DailyPlan plan = {{-1}, {-1}};

    const auto res = simulateDay(s, st, plan);
    assert(res.valid);
    assert(res.state.hasAcquired(0, 0));
    assert(!res.state.hasAcquired(1, 0));
}

void testFuelValidation() {
    const MatchSetting s = makeSetting();
    MatchState st = makeState();
    st.agents[0].fuel = 0;

    // step 0 ではまだ補給フェーズを経ていないため、即移動は不正。
    const SimulationResult invalid =
        simulateDay(s, st, {{2, -1}, {2, -1}});
    assert(!invalid.valid);
    assert(invalid.failure.error == SimulationError::InsufficientFuel);

    // 1ステップ待機して補給を受ければ、その後の移動は可能。
    const SimulationResult valid =
        simulateDay(s, st, {{-1, 2}, {-1, 2}});
    assert(valid.valid);
}

void testHashAndEvaluation() {
    const MatchSetting s = makeSetting();
    const MatchState st = makeState();
    const auto sim = simulateDay(s, st, {{2, -1}, {2, -1}});
    assert(sim.valid);

    std::unordered_set<GameState, GameStateHash, GameStateEqual> states;
    states.insert(sim.state);
    assert(states.count(sim.state) == 1);

    GameState changed = sim.state;
    --changed.agents[0].fuel;
    assert(states.count(changed) == 0);

    EvaluationContext ctx;
    const auto eval = evaluateGameState(sim.state, ctx);
    assert(eval.newBrandCount == 1);
    assert(eval.dailyBrandCount == 1);
    assert(eval.udonCount == 1);
}

void testDijkstra() {
    MatchSetting s = makeSetting();
    s.field.cells[5].type = CellType::Road;

    MatchState st = makeState();
    st.trafficByPos[5] = TrafficStatus::Smooth;

    // 4(平地) -> 5(道路) -> 2。移動元コストなので 2 + 1 = 3。
    const DijkstraTable table = brain0(s, st, 4);
    assert(table.canReach(2));
    assert(table.distances[2] == 3);

    const PathResult patrol =
        Brain0::restorePath(s, table, 2, AgentKind::Patrol);
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
        Brain0::restorePath(s, table, 2, AgentKind::Supply);
    assert(supply.reachable);
    assert(supply.cost.fuel == 0);

    // 直接経路を池で塞ぐと、4 -> 1 -> 2 の迂回路になる。
    s.field.cells[5].type = CellType::Pond;
    const PathResult detour = Brain0::shortestPath(s, st, 4, 2);
    assert(detour.reachable);
    assert(detour.cost.steps == 4);
    assert((detour.positions == std::vector<CellPos>{4, 1, 2}));

    const PathResult invalid =
        Brain0::shortestPath(s, st, kInvalidCell, 2);
    assert(!invalid.reachable);
}

void testBeamSearch() {
    const MatchSetting s = makeSetting();
    const MatchState st = makeState();

    BeamSearchConfig cfg;
    cfg.beamWidth = 64;
    cfg.jointBeamWidth = 256;
    const auto res = brain1(s, st, {}, cfg);

    assert(res.success);
    assert(res.plan.size() == st.agents.size());
    assert(res.finalState.finalized);
    assert(res.finalState.dayUdon == 1);
    assert(validateDailyPlan(s, st, res.plan).valid);
}

void testAgentSelector() {
    const MatchSetting s = makeSetting();
    AgentSelectorConfig cfg;
    cfg.trialCandidateLimit = 2;
    cfg.trialBeamWidth = 16;
    cfg.trialJointBeamWidth = 64;

    const auto first = selectAgentKinds(s, cfg);
    const auto again = selectAgentKinds(s, cfg);
    assert(first.success);
    assert(first.kinds == again.kinds);
    assert(first.kinds.size() == s.agentCount());
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

    cfg.minSupplyCount = 1;
    cfg.maxSupplyCount = 1;
    cfg.trialCandidateLimit = 0;
    const auto fixed = selectAgentKinds(s, cfg);
    assert(fixed.success);
    assert(fixed.supplyCount == 1);
    assert(fixed.evaluatedAssignments == 2);
    assert(fixed.trialAssignments == 0);
}

void testProblemCreator() {
    CreateConfig cfg;
    cfg.width = 8;
    cfg.height = 8;
    cfg.agentCount = 3;
    cfg.spotCount = 8;
    cfg.brandCount = 4;
    cfg.dayCount = 4;
    cfg.teamCount = 2;
    cfg.seed = 123456789;

    const Field terrain = ProblemCreator::generateField(cfg);
    const GeneratedProblem first = ProblemCreator::generate(cfg);
    const GeneratedProblem second = ProblemCreator::generate(cfg);
    const std::string json = ProblemCreator::toJson(first);

    assert(json == ProblemCreator::toJson(second));
    assert(first.setting.field.cells.size() == 64);
    assert(terrain.cells.size() == 64);
    assert(terrain.spots.empty());
    for (std::size_t i = 0; i < terrain.cells.size(); ++i) {
        assert(terrain.cells[i].type == first.setting.field.cells[i].type);
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

    MatchSetting s = first.setting;
    s.startsAt = 1'800'000'000;
    const std::string setJson = ProblemCreator::toMatchSettingJson(s);
    assert(setJson.find("\"startsAt\": 1800000000") !=
           std::string::npos);
    assert(setJson.find("\"map\"") != std::string::npos);
    assert(setJson.find("\"spots\"") != std::string::npos);
    assert(setJson.find("\"agents\"") != std::string::npos);
    assert(setJson.find("\"players\": 2") != std::string::npos);
    assert(setJson.find("\"problem\"") == std::string::npos);
    assert(createMatchSettingJson(
        s, "/tmp/procon37-match-setting-test.json"));

    CreateMatchStateConfig stateCfg;
    stateCfg.day = 0;
    stateCfg.endsAt = 1'800'000'060;
    stateCfg.supplyCount = 1;
    stateCfg.seed = 987654321;
    const MatchState st = ProblemCreator::generateMatchState(s, stateCfg);
    assert(st.day == 0);
    assert(st.endsAt == stateCfg.endsAt);
    assert(st.agents.size() == s.agentCount());
    assert(st.others.size() == static_cast<std::size_t>(s.playerCount - 1));
    for (std::size_t i = 0; i < st.agents.size(); ++i) {
        assert(st.agents[i].index == i);
        assert(st.agents[i].pos == s.agentStarts[i]);
        assert(st.agents[i].fuel == s.fuelLimit);
    }
    assert(st.agents.back().kind == AgentKind::Supply);
    for (const TrafficStatus traffic : st.trafficByPos) {
        assert(traffic == TrafficStatus::Smooth);
    }

    const std::string stateJson =
        ProblemCreator::toMatchStateJson(st, s.field);
    assert(stateJson.find("\"endsAt\": 1800000060") !=
           std::string::npos);
    assert(stateJson.find("\"day\": 0") != std::string::npos);
    assert(stateJson.find("\"others\"") != std::string::npos);
    assert(stateJson.find("\"traffics\"") != std::string::npos);
    assert(createMatchStateJson(
        st,
        s.field,
        "/tmp/procon37-match-state-test.json"));

    const MatchState direct = createMatchStateJson(
        s,
        stateCfg,
        "/tmp/procon37-match-state-direct-test.json");
    assert(ProblemCreator::toMatchStateJson(direct, s.field) == stateJson);

    stateCfg.day = 1;
    const MatchState later = ProblemCreator::generateMatchState(s, stateCfg);
    for (const Agent& agent : later.agents) {
        assert(s.field.contains(agent.pos));
        assert(s.field.at(agent.pos).isPassable());
        assert(agent.fuel >= 0);
        assert(agent.fuel <= s.fuelLimit);
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
