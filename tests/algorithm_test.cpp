#include "model.hpp"

#include <cassert>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>

using namespace benzen;

namespace {

MatchSetting line(int width, std::vector<int> days, int fuel, std::vector<CellPos> starts,
                  std::vector<Spot> spots) {
    MatchSetting s;
    s.field.width = width;
    s.field.height = 1;
    s.field.cells.assign(width, {CellType::Plain, -1});
    s.field.spots = std::move(spots);
    s.agentStarts = std::move(starts);
    s.daySteps = std::move(days);
    s.daySeconds.assign(s.daySteps.size(), 60);
    s.fuelLimit = fuel;
    s.playerCount = 2;
    assert(s.field.rebuildSpotIndex());
    return s;
}

MatchState stateFor(const MatchSetting& s, std::vector<AgentKind> kinds) {
    MatchState state;
    state.trafficByPos.assign(s.field.cellCount(), TrafficStatus::Smooth);
    for (AgentIndex a = 0; a < kinds.size(); ++a)
        state.agents.push_back({a, kinds[a], s.agentStarts[a], s.fuelLimit});
    return state;
}

BeamSearchConfig smallBeam() {
    BeamSearchConfig cfg;
    cfg.beamWidth = 64;
    cfg.jointBeamWidth = 128;
    cfg.threadCount = 1;
    cfg.useStateDeadline = false;
    return cfg;
}

void testCompleteIncumbentRoutes() {
    auto s = line(3, {6}, 1, {0, 1}, {{1, 2, 1, -1}});
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    GameState state;
    assert(state.initialize(s, ms));
    assert(tryScheduleAction(state, s, ms, 0, 2).error == SimulationError::None);
    assert(tryScheduleAction(state, s, ms, 1, -6).error == SimulationError::None);
    assert(advanceSimulationStep(state, s).error == SimulationError::None);
    assert(state.pendingActions[0].remainingSteps == 1);
    ActionRoutes routes;
    std::array<ActionRoutes::Id, kMaxAgents> tails;
    tails.fill(ActionRoutes::none);
    // 予約中の移動後、補給してもう1マス。最後の盤外移動は待機へ置換。
    tails[0] = routes.intern({2, 2});
    DailyPlan malformed;
    assert(!appendRemainingRoutes(state, tails, routes, s, ms, malformed, [] { return false; }));
    DailyPlan plan{{2}, {-6}};
    assert(appendRemainingRoutes(state, tails, routes, s, ms, plan, [] { return false; }));
    const auto check = simulateDay(s, ms, plan);
    assert(check.valid && check.state.score.totalUdon == 1);
    assert(check.state.agents[0].pos == 2);
    DailyPlan stopped{{2}, {-6}};
    assert(!appendRemainingRoutes(state, tails, routes, s, ms, stopped, [] { return true; }));
    assert(stopped == DailyPlan({{2}, {-6}}));

    // 補給車不在で残燃料が足りなければ、実行不能な経路は送信しない。
    s.agentStarts.resize(1);
    ms = stateFor(s, {AgentKind::Patrol});
    assert(state.initialize(s, ms));
    assert(tryScheduleAction(state, s, ms, 0, 2).error == SimulationError::None);
    plan = {{2}};
    assert(!appendRemainingRoutes(state, tails, routes, s, ms, plan, [] { return false; }));
}

void testCompletionPreservesIncumbentScore() {
    for (std::uint64_t seed = 61; seed <= 64; ++seed) {
        CreateConfig create;
        create.width = create.height = 8;
        create.agentCount = 3;
        create.spotCount = 8;
        create.brandCount = 5;
        create.seed = seed;
        auto s = ProblemCreator::generate(create).setting;
        s.fuelLimit = std::max(1, s.daySteps.front() / 2);
        auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol, AgentKind::Supply});
        for (std::size_t c = 0; c < ms.trafficByPos.size(); ++c)
            ms.trafficByPos[c] = static_cast<TrafficStatus>((c + seed) % 3);
        auto cfg = smallBeam();
        cfg.beamWidth = 8;
        cfg.jointBeamWidth = 24;
        cfg.adaptiveBeamWidth = false;
        cfg.completeIncumbentRoutes = false;
        const auto baseline = brain1(s, ms, {}, cfg);
        cfg.fuelAwareWeights = false;
        const auto unweighted = brain1(s, ms, {}, cfg);
        assert(unweighted.plan == baseline.plan); // 無期限では短時間用の重みを使わない。
        cfg.fuelAwareWeights = true;
        cfg.completeIncumbentRoutes = true;
        const auto completed = brain1(s, ms, {}, cfg);
        assert(baseline.success && completed.success);
        assert(trialScore(completed.finalState.score) >= trialScore(baseline.finalState.score));
        assert(completed.expandedNodes == baseline.expandedNodes);
        const auto replay = simulateDay(s, ms, completed.plan);
        assert(replay.valid && replay.state == completed.finalState);
    }
}

void testFinalDayRoutePortfolio() {
    // 小中盤面でも定期再計画は、通常経路だけで逃した取得を回収する。
    // 最終日以外は同じ経路を使い、翌日開始位置への影響を避ける。
    for (const int size : {16, 24}) {
        CreateConfig create;
        create.width = create.height = size;
        create.agentCount = size == 16 ? 4 : 8;
        create.spotCount = size;
        create.brandCount = size * 3 / 4;
        create.seed = size == 16 ? 2 : 23;
        auto setting = ProblemCreator::generate(create).setting;
        if (size == 24) setting.fuelLimit = setting.daySteps.front() / 2;
        MatchRunnerConfig config;
        config.beam = smallBeam();
        config.beam.beamWidth = 32;
        config.beam.adaptiveBeamWidth = false;
        config.beam.comparePeriodicReplanPlans = false;
        config.trafficByDay.assign(setting.dayCount(),
            std::vector<TrafficStatus>(setting.field.cellCount()));
        for (std::size_t day = 0; day < setting.dayCount(); ++day)
            for (std::size_t pos = 0; pos < setting.field.cellCount(); ++pos)
                config.trafficByDay[day][pos] = static_cast<TrafficStatus>(
                    (pos + create.seed + day) % 3);
        std::vector<AgentKind> kinds(create.agentCount, AgentKind::Patrol);
        kinds.back() = AgentKind::Supply;
        const auto ordinary = runMatch(setting, kinds, config);
        assert(ordinary.success);
        const auto& last = ordinary.days.back();
        const auto previous = ordinary.days[ordinary.days.size() - 2].endState.score;
        config.beam.comparePeriodicReplanPlans = true;
        const auto portfolio = brain1(setting, last.startState, previous, config.beam);
        config.beam.macroReplanInterval = 4;
        const auto periodic = brain1(setting, last.startState, previous, config.beam);
        assert(portfolio.success && periodic.success);
        assert(trialScore(portfolio.finalState.score) > trialScore(ordinary.score));
        assert(trialScore(portfolio.finalState.score) >= trialScore(periodic.finalState.score));
        const auto replay = simulateDay(setting, last.startState, portfolio.plan, previous);
        assert(replay.valid && replay.state == portfolio.finalState);
        config.beam.macroReplanInterval = 0;
        const auto intermediate = brain1(setting, ordinary.days.front().startState,
                                         {}, config.beam);
        assert(intermediate.plan == ordinary.days.front().plan);
    }
}

void testDialAgainstHeap() {
    CreateConfig cfg;
    cfg.width = cfg.height = 8;
    cfg.agentCount = 3;
    cfg.spotCount = 8;
    cfg.brandCount = 5;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        cfg.seed = seed;
        const auto s = ProblemCreator::generate(cfg).setting;
        auto ms = stateFor(s, std::vector<AgentKind>(3, AgentKind::Patrol));
        for (std::size_t i = 0; i < ms.trafficByPos.size(); ++i)
            ms.trafficByPos[i] = static_cast<TrafficStatus>((i + seed) % 3);
        for (CellPos start = 0; start < static_cast<CellPos>(s.field.cellCount()); ++start) {
            const auto table = brain0(s, ms, start);
            std::vector<int> expected(s.field.cellCount(), kImpassableCost);
            using Item = std::pair<int, CellPos>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
            if (s.field.at(start).isPassable()) {
                expected[start] = 0;
                queue.emplace(0, start);
            }
            while (!queue.empty()) {
                const auto [distance, from] = queue.top();
                queue.pop();
                if (distance != expected[from])
                    continue;
                for (const CellPos to : s.field.neighbors(from)) {
                    if (!s.field.contains(to) || !s.field.at(to).isPassable())
                        continue;
                    const int next = distance + moveStepCost(s, ms, from);
                    if (next >= expected[to])
                        continue;
                    expected[to] = next;
                    queue.emplace(next, to);
                }
            }
            assert(table.distances == expected);
            for (CellPos goal = 0; goal < static_cast<CellPos>(s.field.cellCount()); ++goal) {
                if (!table.canReach(goal) || goal == start)
                    continue;
                const auto path = Brain0::restorePath(s, table, goal);
                assert(path.reachable && path.positions[1] == table.firstHop[goal]);
                int cost = 0;
                for (std::size_t i = 1; i < path.positions.size(); ++i)
                    cost += moveStepCost(s, ms, path.positions[i - 1]);
                assert(cost == expected[goal]);
            }
        }
    }
}

void testFlowAgainstEnumeration() {
    std::mt19937 random(19);
    constexpr int n = 4, sink = 2 * n + 1;
    MinCostFlow flow(0);
    for (int test = 0; test < 40; ++test) {
        flow.reset(sink + 1);
        std::array<std::array<int, n>, n> cost{};
        for (int a = 0; a < n; ++a) {
            flow.addEdge(0, a + 1, 1, 0);
            flow.addEdge(a + 1, sink, 1, 0);
            flow.addEdge(n + 1 + a, sink, 1, 0);
            for (int b = 0; b < n; ++b) {
                cost[a][b] = static_cast<int>(random() % 41) - 20;
                flow.addEdge(a + 1, n + 1 + b, 1, cost[a][b]);
            }
        }
        std::function<int(int, unsigned)> enumerate = [&](int a, unsigned used) {
            if (a == n)
                return 0;
            int best = enumerate(a + 1, used);
            for (int b = 0; b < n; ++b)
                if ((used & (1U << b)) == 0)
                    best = std::min(best, cost[a][b] + enumerate(a + 1, used | (1U << b)));
            return best;
        };
        const auto result = flow.run(0, sink, n);
        assert(result.first == n && result.second == enumerate(0, 0));
    }
    // 頂点数を縮小・拡大しても前の辺や最短路作業領域を引き継がない。
    for (const int vertices : {2, 12, 3, 9}) {
        flow.reset(vertices);
        flow.reserveEdges(0, 4);
        flow.addEdge(0, vertices - 1, 2, -7);
        const auto first = flow.run(0, vertices - 1, 1);
        const auto second = flow.run(0, vertices - 1, 2);
        assert(first.first == 1 && first.second == -7);
        assert(second.first == 1 && second.second == -7);
    }
}

void testSearchEquivalence() {
    const auto s = line(3, {6}, 4, {0, 1}, {{1, 2, 1, -1}});
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    GameState a;
    assert(a.initialize(s, ms));
    assert(tryScheduleAction(a, s, ms, 0, -4).error == SimulationError::None);
    GameState b = a;
    b.pendingActions[0].rawAction = -6;
    b.agents[1].fuel = 100;
    assert(a != b);
    assert(canonicalSearchState(a) == canonicalSearchState(b));
    b.agents[0].fuel = 2;
    assert(searchStateDominates(a, b));
    assert(!searchStateDominates(b, a));
    b.acquiredSpots[0] = spotBit(0);
    assert(!searchStateDominates(a, b));
    b = a;
    --b.pendingActions[0].remainingSteps;
    assert(!searchStateDominates(a, b));
}

// 燃料は移動ごとに増えるため、使用燃料順のDPで最短路を独立検証できる。
void testFuelConstrainedAgainstDP() {
    for (int seed = 1; seed <= 3; ++seed) {
        CreateConfig create;
        create.width = create.height = 8;
        create.agentCount = 3;
        create.spotCount = 8;
        create.brandCount = 5;
        create.seed = seed;
        const auto s = ProblemCreator::generate(create).setting;
        auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply, AgentKind::Patrol});
        for (std::size_t i = 0; i < ms.trafficByPos.size(); ++i)
            ms.trafficByPos[i] = static_cast<TrafficStatus>((i + seed) % 3);
        const auto start = s.agentStarts[0];
        constexpr int budget = 6;
        std::array<std::vector<int>, budget + 1> dp;
        for (auto& row : dp)
            row.assign(s.field.cellCount(), kImpassableCost);
        dp[0][start] = 0;
        for (int used = 0; used <= budget; ++used) {
            for (CellPos from = 0; from < static_cast<CellPos>(s.field.cellCount()); ++from) {
                if (dp[used][from] >= kImpassableCost)
                    continue;
                const int fuel = moveFuelCost(s, AgentKind::Patrol, from);
                if (fuel > budget - used)
                    continue;
                for (int d = 0; d < 6; ++d) {
                    const auto to = s.field.neighbor(from, static_cast<Direction>(d));
                    if (!s.field.contains(to) || !s.field.at(to).isPassable())
                        continue;
                    dp[used + fuel][to] =
                        std::min(dp[used + fuel][to], dp[used][from] + moveStepCost(s, ms, from));
                }
            }
        }
        for (CellPos goal = 0; goal < static_cast<CellPos>(s.field.cellCount()); ++goal) {
            if (!s.field.at(goal).isPassable())
                continue;
            for (int limit = 0; limit <= budget; ++limit) {
                std::pair<int, int> expected{kImpassableCost, 0};
                for (int used = 0; used <= limit; ++used)
                    expected = std::min(expected, std::make_pair(dp[used][goal], used));
                const auto path =
                    brain0FuelConstrained(s, ms, start, goal, AgentKind::Patrol, limit);
                assert(path.reachable == (expected.first < kImpassableCost));
                if (!path.reachable)
                    continue;
                assert(path.cost.steps == expected.first && path.cost.fuel == expected.second);
                int steps = 0, fuel = 0;
                CellPos pos = start;
                for (const auto direction : path.directions) {
                    steps += moveStepCost(s, ms, pos);
                    fuel += moveFuelCost(s, AgentKind::Patrol, pos);
                    pos = s.field.neighbor(pos, direction);
                    assert(s.field.contains(pos) && s.field.at(pos).isPassable());
                }
                assert(pos == goal && steps == expected.first && fuel == expected.second);
            }
        }
    }
}

void testFuelTimeTradeoff() {
    auto s = line(3, {4}, 2, {0}, {});
    s.field.height = 2;
    s.field.cells.assign(6, {CellType::Pond, -1});
    s.field.cells[0].type = s.field.cells[4].type = s.field.cells[5].type = CellType::Plain;
    s.field.cells[1].type = CellType::Road;
    s.field.spots = {{1, 5, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    auto ms = stateFor(s, {AgentKind::Patrol});
    const auto shortest = brain0(s, ms, 0, 5);
    assert(shortest.cost.steps == 3 && shortest.cost.fuel == 3);
    for (const bool macros : {false, true}) {
        auto cfg = smallBeam();
        cfg.useMacroActions = macros;
        const auto result = brain1(s, ms, {}, cfg);
        assert(result.success && result.finalState.dayUdon == 1);
        assert(result.finalState.agents[0].fuel == 0);
        assert(validateDailyPlan(s, ms, result.plan).valid);
        if (macros) {
            assert(result.macroCandidates > 0);
            assert(result.macroContinuations > 0);
        }
    }
    s.daySteps = {3};
    const auto impossible = brain1(s, ms, {}, smallBeam());
    assert(impossible.success && impossible.finalState.dayUdon == 0);
}

void testTimedRefuelPathAndGreedyBaseline() {
    auto s = line(3, {5}, 1, {0, 2}, {{1, 2, 1, -1}});
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    assert(!brain0FuelConstrained(s, ms, 0, 2, AgentKind::Patrol, 1).reachable);
    const auto timed = brain0TimedRefuels(s, ms, 0, 2, AgentKind::Patrol, 1, 1, 0, 5, {{1, 3}});
    assert(timed.reachable);
    assert(timed.arrivalStep == 4 && timed.cost.steps == 4);
    assert(timed.cost.fuel == 2 && timed.finalFuel == 0);
    assert(timed.refuelCount == 1);
    assert((timed.positions == std::vector<CellPos>{0, 1, 2}));
    assert(timed.actions.size() == 2);
    assert(std::count_if(timed.actions.begin(), timed.actions.end(),
                         [](int action) { return isMoveAction(action); }) == 2);

    AgentPlan patrol = timed.actions;
    patrol.push_back(-1);
    DailyPlan coordinated{
        patrol,
        {-1, static_cast<int>(Direction::Left), -2},
    };
    const auto refueled = simulateDay(s, ms, coordinated);
    assert(refueled.valid && refueled.state.dayUdon == 1);
    assert(refueled.state.agents[0].pos == 2);

    s = line(5, {8}, 10, {0, 0}, {{1, 2, 1, -1}, {2, 4, 1, -1}});
    ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    const auto greedy = brain0Greedy(s, ms);
    const auto baseline = simulateDay(s, ms, greedy);
    assert(baseline.valid && baseline.state.dayUdon == 2);
    assert(baseline.state.agents[0].pos == 4);
    assert((greedy[1] == AgentPlan{-8}));
}

void testTrafficNeverSchedulesAnUnfinishableMove() {
    auto s = line(2, {3}, 2, {0}, {{1, 1, 1, -1}});
    s.field.cells[0].type = CellType::Road;
    assert(s.field.rebuildSpotIndex());
    auto ms = stateFor(s, {AgentKind::Patrol});
    ms.trafficByPos[0] = TrafficStatus::Jammed;

    GameState state;
    assert(state.initialize(s, ms));
    assert(tryScheduleAction(state, s, ms, 0, static_cast<std::int32_t>(Direction::Right)).error ==
           SimulationError::ActionOverrunsDay);

    const auto jammed = brain1(s, ms, {}, smallBeam());
    assert(jammed.success);
    assert(validateDailyPlan(s, ms, jammed.plan).valid);
    assert(std::none_of(jammed.plan[0].begin(), jammed.plan[0].end(),
                        [](std::int32_t action) { return isMoveAction(action); }));

    ms.trafficByPos[0] = TrafficStatus::Busy;
    const auto busy = brain1(s, ms, {}, smallBeam());
    assert(busy.success);
    assert(validateDailyPlan(s, ms, busy.plan).valid);
    assert(busy.finalState.dayUdon == 1);
}

void testRendezvousObstaclesAndMultipleSupplies() {
    auto s = line(5, {6}, 1, {0, 10}, {});
    s.field.height = 3;
    s.field.cells.assign(15, {CellType::Plain, -1});
    for (CellPos pos = 5; pos < 9; ++pos)
        s.field.cells[pos].type = CellType::Pond;
    s.field.spots = {{1, 2, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    assert(brain0(s, ms, 10, 1, AgentKind::Supply).cost.steps > 6);
    auto cfg = smallBeam();
    const auto result = brain1(s, ms, {}, cfg);
    assert(result.success && result.finalState.dayUdon == 0);
    assert(validateDailyPlan(s, ms, result.plan).valid);

    s = line(7, {4}, 1, {2, 4, 1, 5}, {{1, 0, 1, -1}, {2, 6, 1, -1}});
    ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol, AgentKind::Supply, AgentKind::Supply});
    const auto two = brain1(s, ms, {}, cfg);
    assert(two.success && two.finalState.dayUdon == 2);
    assert(validateDailyPlan(s, ms, two.plan).valid);
    cfg.threadCount = 2;
    cfg.parallelThreshold = 1;
    const auto parallel = brain1(s, ms, {}, cfg);
    assert(two.plan == parallel.plan);
}

void testMultipleRefuelsInOneDay() {
    const auto s = line(6, {10}, 2, {0, 0}, {{1, 5, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    auto cfg = smallBeam();
    cfg.beamWidth = 128;
    cfg.jointBeamWidth = 512;
    const auto result = brain1(s, ms, {}, cfg);
    assert(result.success);
    assert(validateDailyPlan(s, ms, result.plan).valid);
    assert(result.finalState.dayUdon == 1);
    assert(result.finalState.agents[0].pos == 5);
    // 5燃料分を燃料上限2で走るため、少なくとも2回の補給が必要。
    assert(result.finalState.agents[0].fuel == s.fuelLimit);
    // 経路を途中で再検討しても、補給後の燃料と合法性を維持する。
    cfg.macroReplanInterval = 1;
    const auto replanned = brain1(s, ms, {}, cfg);
    assert(replanned.success && replanned.finalState.dayUdon == 1);
    assert(validateDailyPlan(s, ms, replanned.plan).valid);
}

void testSupplyMovesOnlyForRendezvous() {
    // 目的地へは届くが、到着時に燃料が尽きる。補給車も移動して
    // 合流し、次の巡回に使える燃料を残す。
    auto s = line(5, {8}, 4, {0, 0}, {{1, 4, 1, -1}});
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    auto cfg = smallBeam();
    cfg.beamWidth = 1;
    cfg.jointBeamWidth = 1;
    cfg.maxActionsPerAgent = 1;
    cfg.adaptiveBeamWidth = false;
    const auto escorted = brain1(s, ms, {}, cfg);
    assert(escorted.success && escorted.finalState.dayUdon == 1);
    assert(validateDailyPlan(s, ms, escorted.plan).valid);
    assert(std::any_of(escorted.plan[1].begin(), escorted.plan[1].end(),
                       [](std::int32_t action) { return isMoveAction(action); }));
    assert(escorted.finalState.agents[0].fuel > 0);

    // 緊急補給が不要でも、巡回車を待たせず目的スポットへ先着できる
    // 補給車は同行し、次の巡回に備えて合流する。
    s = line(5, {8}, 10, {0, 0}, {{1, 4, 1, -1}});
    ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    const auto standby = brain1(s, ms, {}, cfg);
    assert(standby.success && standby.finalState.dayUdon == 1);
    assert(validateDailyPlan(s, ms, standby.plan).valid);
    assert(standby.finalState.agents[0].pos == 4);
    assert(standby.finalState.agents[1].pos == 4);
    assert(standby.finalState.agents[0].fuel == s.fuelLimit);

    // 補給が不要なら補給車は無目的に移動しない。
    s = line(3, {4}, 6, {0, 2}, {});
    ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    const auto idle = brain1(s, ms, {}, cfg);
    assert(idle.success && idle.finalState.dayUdon == 0);
    assert(validateDailyPlan(s, ms, idle.plan).valid);
    assert(std::none_of(idle.plan[1].begin(), idle.plan[1].end(),
                        [](std::int32_t action) { return isMoveAction(action); }));

    // 同じ地点へ戻るだけの補給車の周回は、終端状態を変えず待機へ置換する。
    s = line(3, {4}, 6, {0, 1}, {});
    ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    DailyPlan wandering{{-4}, {2, 5}};
    const auto before = simulateDay(s, ms, wandering);
    assert(before.valid);
    assert(Brain1::simplifySupplyPlan(
               s, ms, {}, before.state, wandering) == 1);
    assert(std::none_of(wandering[1].begin(), wandering[1].end(),
                        [](std::int32_t action) { return isMoveAction(action); }));
    const auto after = simulateDay(s, ms, wandering);
    assert(after.valid && after.state == before.state);
}

void testSupplyReplansInsteadOfWaitingAllDay() {
    // 32x32・seed 1では、巡回車がマクロ経路へ入った後に通常の巡回先割当が
    // 一時的に消える。補給車が残り全時間を待機せず、移動中の巡回車を
    // 追跡して次の合流を再計画することを確認する。
    CreateConfig create;
    create.width = create.height = 32;
    create.agentCount = 8;
    create.spotCount = 32;
    create.brandCount = 20;
    create.dayCount = 10;
    create.seed = 1;
    const auto s = ProblemCreator::generate(create).setting;
    const auto ms = stateFor(s, {
        AgentKind::Patrol, AgentKind::Patrol, AgentKind::Patrol,
        AgentKind::Patrol, AgentKind::Patrol,
        AgentKind::Supply, AgentKind::Supply, AgentKind::Supply,
    });
    auto cfg = smallBeam();
    cfg.beamWidth = 128;
    cfg.jointBeamWidth = 512;
    const auto result = brain1(s, ms, {}, cfg);
    assert(result.success);
    assert(validateDailyPlan(s, ms, result.plan).valid);
    const auto moveCount = [&](AgentIndex agent) {
        return std::count_if(result.plan[agent].begin(), result.plan[agent].end(),
            [](std::int32_t action) { return isMoveAction(action); });
    };
    // 修正前は両方とも3回移動後、残り140/138ステップを待機していた。
    assert(moveCount(5) > 3);
    assert(moveCount(6) > 3);
}

void testCoordinatedPlanningAndCache() {
    auto s = line(7, {8}, 2, {0, 6, 3}, {{1, 2, 1, -1}, {2, 4, 1, -1}, {3, 0, 1, -1}});
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol, AgentKind::Supply});
    auto cfg = smallBeam();
    const auto cached = brain1(s, ms, {}, cfg);
    cfg.assignmentCacheLimit = 0;
    const auto uncached = brain1(s, ms, {}, cfg);
    assert(cached.success && uncached.success);
    assert(cached.plan == uncached.plan);
    assert(cached.finalState == uncached.finalState);
    assert(cached.assignmentCacheHits > 0);
    assert(uncached.assignmentCacheHits == 0);
    assert(cached.assignmentComputations < uncached.assignmentComputations);
    cfg.assignmentCacheLimit = 1; // 小容量での消去も結果を変えない。
    cfg.threadCount = 2;
    cfg.parallelThreshold = 1;
    const auto tiny = brain1(s, ms, {}, cfg);
    assert(tiny.plan == cached.plan);
    assert(validateDailyPlan(s, ms, tiny.plan).valid);

    s = line(5, {8}, 2, {0, 4, 2}, {{1, 1, 1, -1}, {2, 3, 1, -1}});
    ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol, AgentKind::Supply});
    ms.agents[0].fuel = ms.agents[1].fuel = 0;
    cfg = smallBeam();
    const auto conflict = brain1(s, ms, {}, cfg);
    assert(conflict.success && conflict.repairedAssignments > 0);
    assert(validateDailyPlan(s, ms, conflict.plan).valid);
}

void testDistanceCache() {
    auto s = line(7, {10, 10}, 6, {0, 6}, {{1, 2, 1, -1}, {2, 5, 1, -1}});
    s.field.cells[1].type = CellType::Road;
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    auto cfg = smallBeam();
    cfg.distanceCacheEntries = 0;
    const auto uncached = brain1(s, ms, {}, cfg);
    cfg.distanceCacheEntries = 4;
    const auto cold = brain1(s, ms, {}, cfg);
    const auto warm = brain1(s, ms, {}, cfg);
    assert(!cold.reusedDistanceTables && warm.reusedDistanceTables);
    assert(cold.plan == uncached.plan && warm.plan == uncached.plan);
    assert(warm.finalState == uncached.finalState);

    // 燃料や得点は距離表のキーに含めず、評価・取得を毎回やり直す。
    ms.agents[0].fuel = 1;
    ScoreState prev;
    prev.matchBrands = brandBit(0);
    const auto changedState = brain1(s, ms, prev, cfg);
    assert(changedState.reusedDistanceTables);
    cfg.distanceCacheEntries = 0;
    const auto freshState = brain1(s, ms, prev, cfg);
    assert(changedState.plan == freshState.plan);
    assert(changedState.finalState == freshState.finalState);
    cfg.distanceCacheEntries = 4;
    (void)brain1(s, ms, {}, cfg);

    auto checkMiss = [&]() {
        const auto changed = brain1(s, ms, {}, cfg);
        assert(!changed.reusedDistanceTables);
        assert(changed.success && validateDailyPlan(s, ms, changed.plan).valid);
        cfg.distanceCacheEntries = 0;
        const auto fresh = brain1(s, ms, {}, cfg);
        assert(changed.plan == fresh.plan && changed.finalState == fresh.finalState);
        cfg.distanceCacheEntries = 4;
        (void)brain1(s, ms, {}, cfg);
    };
    ms.trafficByPos[1] = TrafficStatus::Busy;
    checkMiss();
    // 移動時間はどちらも2だが、燃料消費が違う。
    s.field.cells[1].type = CellType::Plain;
    checkMiss();
    s.field.spots[0].pos = 3;
    assert(s.field.rebuildSpotIndex());
    checkMiss();
    std::swap(s.field.spots[0], s.field.spots[1]);
    assert(s.field.rebuildSpotIndex());
    checkMiss();
    s.field.width = 1;
    s.field.height = 7;
    checkMiss();

    // 容量1では別条件へ切り替えた後、古い距離表を再利用しない。
    cfg.distanceCacheEntries = 1;
    s.field.cells[1].type = CellType::Road;
    (void)brain1(s, ms, {}, cfg);
    ms.trafficByPos[1] = TrafficStatus::Jammed;
    const auto jammed = brain1(s, ms, {}, cfg);
    assert(!jammed.reusedDistanceTables);
    assert(brain1(s, ms, {}, cfg).reusedDistanceTables);
    ms.trafficByPos[1] = TrafficStatus::Busy;
    assert(!brain1(s, ms, {}, cfg).reusedDistanceTables);
}

void testDetourRendezvousAndTwoSpots() {
    // 時間・燃料が等しい目的地経路のうち、添字の小さい経路は
    // 補給車から遠い。下側の経路で合流すれば4ステップで取得できる。
    auto s = line(3, {4}, 1, {0, 3}, {});
    s.field.height = 2;
    s.field.cells.assign(6, {CellType::Plain, -1});
    s.field.spots = {{1, 5, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    auto cfg = smallBeam();
    const auto detour = brain1(s, ms, {}, cfg);
    assert(detour.success && detour.finalState.dayUdon == 1);
    assert(detour.detourPlans > 0);
    assert(validateDailyPlan(s, ms, detour.plan).valid);

    // 8ステップあれば従来の経路上合流も成立するが、取得は6ステップ目。
    // 経路外の合流を比較し、幅1でも4ステップ目に取得する。
    s.daySteps = {8};
    cfg.beamWidth = cfg.jointBeamWidth = cfg.maxActionsPerAgent = 1;
    cfg.adaptiveBeamWidth = false;
    cfg.exhaustFeasibleDetours = false;
    const auto limited = brain1(s, ms, {}, cfg);
    assert(limited.success && validateDailyPlan(s, ms, limited.plan).valid);
    cfg.exhaustFeasibleDetours = true;
    const auto faster = brain1(s, ms, {}, cfg);
    assert(faster.success && faster.finalState.dayUdon == 1);
    assert(validateDailyPlan(s, ms, faster.plan).valid);
    CellPos pos = s.agentStarts[0];
    int arrival = 0;
    for (const int action : faster.plan[0]) {
        if (isWaitAction(action))
            arrival -= action;
        else {
            arrival += moveStepCost(s, ms, pos);
            pos = s.field.neighbor(pos, static_cast<Direction>(action));
        }
        if (pos == 5)
            break;
    }
    assert(pos == 5 && arrival == 4 && faster.detourPlans > 0);
    cfg.threadCount = 2;
    cfg.parallelThreshold = 1;
    assert(brain1(s, ms, {}, cfg).plan == faster.plan);
    cfg = smallBeam();

    s = line(5, {8}, 4, {2}, {{1, 1, 1, -1}, {2, 0, 1, -1}, {3, 4, 1, -1}});
    ms = stateFor(s, {AgentKind::Patrol});
    cfg.beamWidth = 1;
    cfg.jointBeamWidth = 1;
    cfg.maxActionsPerAgent = 1;
    const auto two = brain1(s, ms, {}, cfg);
    assert(two.success && two.finalState.dayUdon >= 2);
    assert(validateDailyPlan(s, ms, two.plan).valid);
}

void testTrialsAndTuning() {
    const auto s = line(3, {2, 4}, 3, {0}, {{1, 0, 1, -1}, {2, 2, 1, -1}});
    auto cfg = smallBeam();
    const auto trial = runTrial(s, {AgentKind::Patrol}, {}, cfg);
    assert(trial.success && trial.days == 2);
    assert((trialScore(trial.score) == TrialScore{2, 3, 3}));
    TrialFirstDay sharedFirstDay;
    TrialTraffic smooth(2, std::vector<TrafficStatus>(3, TrafficStatus::Smooth));
    TrialTraffic changed = smooth;
    changed[1][0] = TrafficStatus::Jammed;
    for (const auto& scenario : {smooth, changed}) {
        const auto direct = runTrial(s, {AgentKind::Patrol}, scenario, cfg);
        const auto reused =
            runTrial(s, {AgentKind::Patrol}, scenario, cfg, 0, kNoTrialDeadline, &sharedFirstDay);
        assert(reused.success && reused.days == direct.days);
        assert(trialScore(reused.score) == trialScore(direct.score));
    }
    const auto expiredCached =
        runTrial(s, {AgentKind::Patrol}, smooth, cfg, 0,
                 std::chrono::steady_clock::now() - std::chrono::seconds(1), &sharedFirstDay);
    assert(!expiredCached.success);
    // 初日の交通まで変わった場合は、以前の結果を流用しない。
    changed[0][0] = TrafficStatus::Busy;
    const auto differentFirstDay =
        runTrial(s, {AgentKind::Patrol}, changed, cfg, 0, kNoTrialDeadline, &sharedFirstDay);
    const auto directFirstDay = runTrial(s, {AgentKind::Patrol}, changed, cfg);
    assert(differentFirstDay.success &&
           trialScore(differentFirstDay.score) == trialScore(directFirstDay.score));
    // 初日だけでなく、分岐する直前までの全日を再利用する。
    auto longMatch = line(5, {3, 4, 5, 6}, 6, {0}, {{1, 0, 1, -1}, {2, 4, 1, -1}});
    longMatch.field.cells[1].type = CellType::Road;
    TrialFirstDay prefix;
    TrialTraffic traffic(4, std::vector<TrafficStatus>(5, TrafficStatus::Smooth));
    bool firstScenario = true;
    for (const int branch : {4, 3, 2, 1, 0, 4}) {
        if (branch < 4)
            traffic[branch][1] = TrafficStatus::Jammed;
        TrialFirstDay fresh;
        const auto direct =
            runTrial(longMatch, {AgentKind::Patrol}, traffic, cfg, 0, kNoTrialDeadline, &fresh);
        const auto cached =
            runTrial(longMatch, {AgentKind::Patrol}, traffic, cfg, 0, kNoTrialDeadline, &prefix);
        assert(cached.success && cached.days == direct.days);
        assert(trialScore(cached.score) == trialScore(direct.score));
        assert(prefix.reusedDays == (firstScenario ? 0U : static_cast<std::size_t>(branch)));
        firstScenario = false;
        assert(prefix.laterDays.size() == 3);
        for (std::size_t d = 0; d < prefix.laterDays.size(); ++d) {
            assert(prefix.laterDays[d].agents[0].pos == fresh.laterDays[d].agents[0].pos);
            assert(prefix.laterDays[d].agents[0].fuel == fresh.laterDays[d].agents[0].fuel);
        }
    }
    // 日数の変更で以前の末尾を参照しない。
    const auto shorter =
        runTrial(longMatch, {AgentKind::Patrol}, traffic, cfg, 2, kNoTrialDeadline, &prefix);
    const auto shorterDirect = runTrial(longMatch, {AgentKind::Patrol}, traffic, cfg, 2);
    assert(shorter.success && prefix.reusedDays == 0 && prefix.laterDays.size() == 1);
    assert(trialScore(shorter.score) == trialScore(shorterDirect.score));
    const std::map<std::uint64_t, TrialScore> groups{{0, {5, 10, 20}}, {1, {5, 10, 20}}};
    auto compensated = groups;
    compensated[0] = {5, 9, 100};
    compensated[1] = {5, 12, 20};
    assert(sumWeightScores(compensated) > sumWeightScores(groups));
    assert(!weightGroupsNotWorse(groups, compensated));
    assert(weightGroupsNotWorse(groups, groups));
    compensated[0] = {5, 10, 21};
    assert(weightGroupsNotWorse(groups, compensated));
    compensated.erase(0);
    assert(!weightGroupsNotWorse(groups, compensated));
    WeightTuningCase train{1, s, {AgentKind::Patrol}, {}};
    WeightTuningCase valid{2, s, {AgentKind::Patrol}, {}};
    WeightTuningConfig tuning;
    tuning.candidates = 2;
    tuning.beam = cfg;
    auto invalidTimed = tuning;
    invalidTimed.fixedWork = false;
    invalidTimed.beam.timeLimitMs = 0;
    bool invalidBudget = false;
    try {
        (void)evaluateWeights({train}, {}, invalidTimed);
    } catch (const std::invalid_argument&) {
        invalidBudget = true;
    }
    assert(invalidBudget);
    const auto first = tuneWeights({train}, {valid}, {}, tuning);
    const auto second = tuneWeights({train}, {valid}, {}, tuning);
    assert(first.evaluatedCandidates == 2);
    assert(first.validationAfter >= first.validationBefore);
    assert(first.trainingAfter == second.trainingAfter);
    assert(first.validationAfter == second.validationAfter);
    bool rejected = false;
    try {
        (void)tuneWeights({train}, {train}, {}, tuning);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    AgentSelectorConfig selection;
    // このテスト盤面はエージェントが1台だけなので、巡回車を残すため
    // 補給車0台を明示的に許可する。
    selection.minSupplyCount = 0;
    selection.maxSupplyCount = 0;
    selection.trialCandidateLimit = 1;
    selection.trialScenarios = {
        TrialTraffic(2, std::vector<TrafficStatus>(3, TrafficStatus::Smooth)),
        TrialTraffic(2, std::vector<TrafficStatus>(3, TrafficStatus::Jammed))};
    const auto selected = selectAgentKinds(s, selection);
    assert(selected.success && selected.trialSimulations == 2);
    assert(selected.completedTrialScenarios == 2);
    assert((selected.worstTrialScore == TrialScore{2, 3, 3}));

    // 期限切れの試走は未完走として扱い、比較スコアへ混ぜない。
    const auto expired = runTrial(s, {AgentKind::Patrol}, {}, cfg, 0,
                                  std::chrono::steady_clock::now() - std::chrono::seconds(1));
    assert(!expired.success && expired.days == 0);

    // 全候補を同じシナリオ数で比較し、選択結果の集計値を再計算できる。
    auto multi = line(7, {4, 5}, 6, {0, 3, 6}, {{1, 0, 1, -1}, {2, 2, 1, -1}, {3, 6, 1, -1}});
    multi.field.cells[1].type = CellType::Road;
    TrialFirstDay changedKindsCache;
    const std::vector<AgentKind> leftSupply{AgentKind::Supply, AgentKind::Patrol,
                                            AgentKind::Patrol};
    const std::vector<AgentKind> rightSupply{AgentKind::Patrol, AgentKind::Patrol,
                                             AgentKind::Supply};
    (void)runTrial(multi, leftSupply, {}, cfg, 0, kNoTrialDeadline, &changedKindsCache);
    const auto changedKinds =
        runTrial(multi, rightSupply, {}, cfg, 0, kNoTrialDeadline, &changedKindsCache);
    const auto directKinds = runTrial(multi, rightSupply, {}, cfg);
    assert(changedKinds.success == directKinds.success);
    assert(trialScore(changedKinds.score) == trialScore(directKinds.score));
    AgentSelectorConfig fair;
    fair.maxSupplyCount = 1;
    fair.trialCandidateLimit = 3;
    fair.trialBeamWidth = 8;
    fair.trialJointBeamWidth = 32;
    fair.trialScenarios = {TrialTraffic(2, std::vector<TrafficStatus>(7, TrafficStatus::Smooth)),
                           TrialTraffic(2, std::vector<TrafficStatus>(7, TrafficStatus::Jammed))};
    const auto choice = selectAgentKinds(multi, fair);
    assert(choice.usedBeamTrial && choice.trialAssignments == 3);
    assert(choice.completedTrialScenarios == 2 && choice.trialSimulations == 6);
    TrialScore total{}, worst;
    worst.fill(std::numeric_limits<std::int64_t>::max());
    BeamSearchConfig verify;
    verify.beamWidth = fair.trialBeamWidth;
    verify.jointBeamWidth = fair.trialJointBeamWidth;
    verify.threadCount = 1;
    for (const auto& traffic : fair.trialScenarios) {
        const auto replay = runTrial(multi, choice.kinds, traffic, verify);
        assert(replay.success);
        const auto score = trialScore(replay.score);
        worst = std::min(worst, score);
        for (std::size_t i = 0; i < total.size(); ++i)
            total[i] += score[i];
    }
    assert(choice.totalTrialScore == total && choice.worstTrialScore == worst);
    for (std::size_t supply = 0; supply < multi.agentCount(); ++supply) {
        std::vector<AgentKind> kinds(multi.agentCount(), AgentKind::Patrol);
        kinds[supply] = AgentKind::Supply;
        TrialScore alternative{};
        for (const auto& traffic : fair.trialScenarios) {
            const auto replay = runTrial(multi, kinds, traffic, verify);
            assert(replay.success);
            const auto score = trialScore(replay.score);
            for (std::size_t i = 0; i < alternative.size(); ++i)
                alternative[i] += score[i];
        }
        assert(choice.totalTrialScore >= alternative);
    }
    fair.timeLimitMs = 60000;
    fair.safetyMarginMs = 0;
    const auto budgeted = selectAgentKinds(multi, fair);
    assert(budgeted.kinds == choice.kinds);
    assert(budgeted.totalTrialScore == choice.totalTrialScore);
    assert(budgeted.completedTrialScenarios == 2);

    auto roads = line(16, {32, 32, 32}, 8, {0, 1, 2, 3}, {{1, 15, 1, -1}});
    for (auto& cell : roads.field.cells)
        cell.type = CellType::Road;
    roads.field.cells[0].type = CellType::Plain;
    roads.busyThreshold = 7;
    roads.jammedThreshold = 9;
    const auto scenarios = makeTrialScenarios(roads);
    assert(scenarios.size() == 3);
    assert(scenarios == makeTrialScenarios(roads));
    for (const auto& scenario : scenarios) {
        assert(std::all_of(scenario[0].begin(), scenario[0].end(),
                           [](TrafficStatus traffic) { return traffic == TrafficStatus::Smooth; }));
        assert(scenario[1][0] == TrafficStatus::Smooth);
    }
    bool lowToMiddle = false;
    bool middleToHigh = false;
    for (std::size_t pos = 0; pos < roads.field.cellCount(); ++pos) {
        assert(scenarios[0][1][pos] <= scenarios[1][1][pos]);
        assert(scenarios[1][1][pos] <= scenarios[2][1][pos]);
        lowToMiddle = lowToMiddle || scenarios[0][1][pos] < scenarios[1][1][pos];
        middleToHigh = middleToHigh || scenarios[1][1][pos] < scenarios[2][1][pos];
    }
    assert(lowToMiddle && middleToHigh);

    const auto baseLoad = predictRoadLoad(roads, 1);
    auto moreAgents = roads;
    moreAgents.agentStarts.insert(moreAgents.agentStarts.end(), {4, 5, 6, 7});
    const auto agentLoad = predictRoadLoad(moreAgents, 1);
    assert(agentLoad.meanPerTeam > baseLoad.meanPerTeam);

    auto moreTeams = roads;
    moreTeams.playerCount = roads.playerCount * 4;
    const auto teamLoad = predictRoadLoad(moreTeams, 1);
    assert(std::abs(teamLoad.meanPerTeam - baseLoad.meanPerTeam) < 1e-9);
    assert(teamLoad.deviationPerTeam < baseLoad.deviationPerTeam);
}

void testConsoleReport() {
    auto s = line(3, {4}, 3, {0}, {{1, 2, 1, -1}});
    s.field.cells[1].type = CellType::Road;
    assert(s.field.rebuildSpotIndex());
    auto ms = stateFor(s, {AgentKind::Patrol});
    ms.trafficByPos[1] = TrafficStatus::Busy;
    const auto result = brain1(s, ms, {}, smallBeam());
    assert(result.success);

    ConsoleConfig config;
    config.traceEveryStep = true;
    std::ostringstream output;
    printConsoleReport(output, s, ms, result, {}, config);
    const std::string text = output.str();
    assert(text.find("=== procon37 solver report ===") != std::string::npos);
    assert(text.find("map 3x1") == std::string::npos);
    assert(text.find("0=plain") == std::string::npos);
    assert(consoleCellToken(s.field, 0, &ms) == "0");
    assert(consoleCellToken(s.field, 1, &ms) == "4");
    assert(consoleCellToken(s.field, 2, &ms) == "[1]");
    assert(text.find("[1]") != std::string::npos);
    assert(text.find("[0]P [pos]0(0,0) [fuel]3/3") != std::string::npos);
    assert(text.find("search\n  success=true") != std::string::npos);
    assert(text.find("macro-candidates=") != std::string::npos);
    assert(text.find("plan\n") != std::string::npos);
    assert(text.find("trace\n") != std::string::npos);
    assert(text.find("evaluation\n  total=") != std::string::npos);
    assert(text.find("result\n  udon=1") != std::string::npos);
    assert(text.find("final map") == std::string::npos);

    DailyPlan invalid(1);
    invalid[0] = {2};
    std::ostringstream error;
    assert(!printConsoleTrace(error, s, ms, invalid));
    assert(error.str().find("trace invalid:") != std::string::npos);

    ms.trafficByPos[1] = TrafficStatus::Jammed;
    std::ostringstream jammed;
    printConsoleMap(jammed, s, &ms);
    assert(consoleCellToken(s.field, 1, &ms) == "5");
    assert(jammed.str().find(" 5 ") != std::string::npos);
}

} // namespace

int main() {
    testCompleteIncumbentRoutes();
    testCompletionPreservesIncumbentScore();
    testFinalDayRoutePortfolio();
    testDialAgainstHeap();
    testFlowAgainstEnumeration();
    testSearchEquivalence();
    testFuelConstrainedAgainstDP();
    testFuelTimeTradeoff();
    testTimedRefuelPathAndGreedyBaseline();
    testTrafficNeverSchedulesAnUnfinishableMove();
    testRendezvousObstaclesAndMultipleSupplies();
    testMultipleRefuelsInOneDay();
    testSupplyMovesOnlyForRendezvous();
    testSupplyReplansInsteadOfWaitingAllDay();
    testCoordinatedPlanningAndCache();
    testDistanceCache();
    testDetourRendezvousAndTwoSpots();
    testTrialsAndTuning();
    testConsoleReport();
}
