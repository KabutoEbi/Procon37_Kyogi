#include "application.hpp"

#include <cassert>
#include <cstddef>
#include <sstream>
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

void testWaitCompression() {
    const AgentPlan raw = {-1, -1, -3, 2, -1, -14};
    const AgentPlan compressed = compressWaitActions(raw);
    assert((compressed == AgentPlan{-5, 2, -15}));

    AgentPlan appended;
    appendPlanAction(appended, -1);
    appendPlanAction(appended, -14);
    assert((appended == AgentPlan{-15}));

    std::ostringstream output;
    printPlan(output, DailyPlan{{-1}, {-1, -14}});
    assert(output.str() == "[\n  [-1],\n  [-15]\n]\n");

    MatchSetting s = makeSetting();
    s.daySteps = {15};
    const MatchState st = makeState();
    const auto rawResult = simulateDay(
        s, st, DailyPlan{{-1, -14}, {-1, -14}});
    const auto compressedResult = simulateDay(
        s, st, DailyPlan{{-15}, {-15}});
    assert(rawResult.valid);
    assert(compressedResult.valid);
    assert(rawResult.state == compressedResult.state);
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
    ctx.nearestNewBrandSpotSteps[0] = 2;
    ctx.nearestDailyBrandSpotSteps[0] = 3;
    ctx.targetSlack = 4;
    ctx.rendezvousMismatch = 5;
    ctx.fuelRisk = 6;
    ctx.unreachableTargetCount = 1;
    const auto eval = evaluateGameState(sim.state, ctx);
    assert(eval.newBrandCount == 1);
    assert(eval.dailyBrandCount == 1);
    assert(eval.udonCount == 1);
    assert(eval.newBrandSpotDistance == 2);
    assert(eval.dailyBrandSpotDistance == 3);
    assert(eval.targetSlack == 4);
    assert(eval.rendezvousMismatch == 5);
    assert(eval.fuelRisk == 6);
    assert(eval.unreachableTargetCount == 1);
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
    assert(res.stopReason == SearchStopReason::Completed);

    cfg.timeLimitMs = 1;
    cfg.safetyMarginMs = 1;
    const auto timed = brain1(s, st, {}, cfg);
    assert(timed.success);
    assert(timed.stopReason == SearchStopReason::TimeLimit);
    assert(timed.elapsedMs >= 0);
    assert(validateDailyPlan(s, st, timed.plan).valid);

    MatchState expired = st;
    expired.endsAt = 1;
    cfg.timeLimitMs = 0;
    cfg.safetyMarginMs = 0;
    const auto deadline = brain1(s, expired, {}, cfg);
    assert(deadline.success);
    assert(deadline.stopReason == SearchStopReason::TimeLimit);
    assert(validateDailyPlan(s, expired, deadline.plan).valid);

    BeamSearchConfig serialCfg;
    serialCfg.beamWidth = 64;
    serialCfg.jointBeamWidth = 256;
    serialCfg.threadCount = 1;
    serialCfg.parallelThreshold = 1;
    const auto serial = brain1(s, st, {}, serialCfg);

    BeamSearchConfig parallelCfg = serialCfg;
    parallelCfg.threadCount = 2;
    const auto parallel = brain1(s, st, {}, parallelCfg);
    assert(serial.success);
    assert(parallel.success);
    assert(serial.plan == parallel.plan);
    assert(serial.evaluation.total == parallel.evaluation.total);
    assert(validateDailyPlan(s, st, parallel.plan).valid);
    for (const AgentPlan& actions : parallel.plan) {
        for (std::size_t i = 1; i < actions.size(); ++i) {
            assert(!(isWaitAction(actions[i - 1]) &&
                     isWaitAction(actions[i])));
        }
    }
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

void testMatchRunner() {
    MatchSetting normal = makeSetting();
    normal.busyThreshold = 2;
    normal.jammedThreshold = 4;
    MatchRunnerConfig plannedCfg;
    plannedCfg.beam.beamWidth = 32;
    plannedCfg.beam.jointBeamWidth = 128;
    const auto planned = runMatch(
        normal, {AgentKind::Patrol, AgentKind::Supply}, plannedCfg);
    assert(planned.success);
    assert(!planned.days[0].usedFallback);
    assert(planned.score.totalUdon == 1);

    MatchSetting s = makeSetting();
    s.startsAt = 1'800'000'000;
    s.daySteps = {1, 1, 1};
    s.daySeconds = {60, 60, 60};
    s.busyThreshold = 2;
    s.jammedThreshold = 4;
    s.field.cells[4].type = CellType::Road;

    MatchRunnerConfig cfg;
    cfg.beam.beamWidth = 0;  // 待機フォールバックを確実に使う。
    cfg.trafficByDay.assign(
        3,
        std::vector<TrafficStatus>(9, TrafficStatus::Smooth));
    cfg.trafficByDay[1][4] = TrafficStatus::Busy;
    cfg.trafficByDay[2][4] = TrafficStatus::Jammed;
    const auto res = runMatch(
        s, {AgentKind::Patrol, AgentKind::Supply}, cfg);

    assert(res.success);
    assert(res.failedDay == -1);
    assert(res.days.size() == 3);
    assert(res.days[0].usedFallback);
    assert(res.days[0].startState.endsAt == 1'800'000'060);
    assert(res.days[1].startState.endsAt == 1'800'000'120);
    assert(res.days[2].startState.endsAt == 1'800'000'180);
    assert(res.days[0].startState.trafficAt(4) == TrafficStatus::Smooth);
    assert(res.days[1].startState.trafficAt(4) == TrafficStatus::Busy);
    assert(res.days[2].startState.trafficAt(4) == TrafficStatus::Jammed);
    for (const MatchDayResult& day : res.days) {
        assert(day.endState.finalized);
        assert(day.roadStaySteps[4] == 2);
        assert(day.endState.agents[0].pos == 4);
        assert(day.endState.agents[1].pos == 4);
    }

    MatchSetting reset = makeSetting();
    reset.daySteps = {1, 1};
    reset.daySeconds = {60, 60};
    reset.busyThreshold = 2;
    reset.jammedThreshold = 4;
    reset.agentStarts = {5, 5};
    MatchRunnerConfig resetCfg = cfg;
    resetCfg.trafficByDay.clear();
    const auto scored = runMatch(
        reset, {AgentKind::Patrol, AgentKind::Supply}, resetCfg);
    assert(scored.success);
    assert(scored.score.totalUdon == 2);
    assert(scored.score.sumOfDailyBrandCounts == 2);
    assert(scored.days[0].endState.dayUdon == 1);
    assert(scored.days[1].endState.dayUdon == 1);
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

    const MatchSetting parsedSetting = parseMatchSettingJson(setJson);
    assert(parsedSetting.startsAt == s.startsAt);
    assert(parsedSetting.daySteps == s.daySteps);
    assert(parsedSetting.daySeconds == s.daySeconds);
    assert(parsedSetting.agentStarts == s.agentStarts);
    assert(parsedSetting.field.width == s.field.width);
    assert(parsedSetting.field.height == s.field.height);
    assert(parsedSetting.field.spots.size() == s.field.spots.size());
    assert(ProblemCreator::toMatchSettingJson(parsedSetting) == setJson);
    const MatchSetting fileSetting = readMatchSettingJson(
        "/tmp/procon37-match-setting-test.json");
    assert(fileSetting.agentStarts == s.agentStarts);

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

    const MatchState parsedState = parseMatchStateJson(stateJson, s);
    assert(parsedState.endsAt == st.endsAt);
    assert(parsedState.day == st.day);
    assert(parsedState.agents.size() == st.agents.size());
    assert(parsedState.others.size() == st.others.size());
    assert(parsedState.trafficByPos == st.trafficByPos);
    for (std::size_t i = 0; i < parsedState.agents.size(); ++i) {
        assert(parsedState.agents[i].index == i);
        assert(parsedState.agents[i].kind == st.agents[i].kind);
        assert(parsedState.agents[i].pos == st.agents[i].pos);
        assert(parsedState.agents[i].fuel == st.agents[i].fuel);
    }
    const MatchState fileState = readMatchStateJson(
        "/tmp/procon37-match-state-test.json", s);
    assert(fileState.trafficByPos == st.trafficByPos);

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

    bool rejected = false;
    try {
        (void)parseMatchSettingJson("{}");
    } catch (const JsonInputError& error) {
        rejected = std::string(error.what()).find("startsAt") !=
                   std::string::npos;
    }
    assert(rejected);
}

}  // namespace

int main() {
    testHexNeighbors();
    testMoveAcquireAndRefuel();
    testAcquisitionOrder();
    testFuelValidation();
    testWaitCompression();
    testHashAndEvaluation();
    testDijkstra();
    testBeamSearch();
    testAgentSelector();
    testMatchRunner();
    testProblemCreator();
}
