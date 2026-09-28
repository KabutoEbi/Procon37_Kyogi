#include "model.hpp"

#include <cassert>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>

using namespace benzen;

namespace {

MatchSetting line(int width, std::vector<int> days, int fuel,
                  std::vector<CellPos> starts, std::vector<Spot> spots) {
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
                const auto [distance, from] = queue.top(); queue.pop();
                if (distance != expected[from]) continue;
                for (const CellPos to : s.field.neighbors(from)) {
                    if (!s.field.contains(to) || !s.field.at(to).isPassable()) continue;
                    const int next = distance + moveStepCost(s, ms, from);
                    if (next >= expected[to]) continue;
                    expected[to] = next;
                    queue.emplace(next, to);
                }
            }
            assert(table.distances == expected);
            for (CellPos goal = 0; goal < static_cast<CellPos>(s.field.cellCount()); ++goal) {
                if (!table.canReach(goal) || goal == start) continue;
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
            if (a == n) return 0;
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

void testRendezvousObstaclesAndMultipleSupplies() {
    auto s = line(5, {6}, 1, {0, 10}, {});
    s.field.height = 3;
    s.field.cells.assign(15, {CellType::Plain, -1});
    for (CellPos pos = 5; pos < 9; ++pos) s.field.cells[pos].type = CellType::Pond;
    s.field.spots = {{1, 2, 1, -1}};
    assert(s.field.rebuildSpotIndex());
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    assert(brain0(s, ms, 10, 1, AgentKind::Supply).cost.steps > 6);
    auto cfg = smallBeam();
    const auto result = brain1(s, ms, {}, cfg);
    assert(result.success && result.finalState.dayUdon == 0);
    assert(validateDailyPlan(s, ms, result.plan).valid);

    s = line(7, {4}, 1, {2, 4, 1, 5}, {{1, 0, 1, -1}, {2, 6, 1, -1}});
    ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol,
                      AgentKind::Supply, AgentKind::Supply});
    const auto two = brain1(s, ms, {}, cfg);
    assert(two.success && two.finalState.dayUdon == 2);
    assert(validateDailyPlan(s, ms, two.plan).valid);
    cfg.threadCount = 2;
    cfg.parallelThreshold = 1;
    const auto parallel = brain1(s, ms, {}, cfg);
    assert(two.plan == parallel.plan);
}

void testMultipleRefuelsInOneDay() {
    const auto s = line(
        6,
        {10},
        2,
        {0, 0},
        {{1, 5, 1, -1}});
    const auto ms = stateFor(
        s, {AgentKind::Patrol, AgentKind::Supply});
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

void testCoordinatedPlanningAndCache() {
    auto s = line(7, {8}, 2, {0, 6, 3},
        {{1, 2, 1, -1}, {2, 4, 1, -1}, {3, 0, 1, -1}});
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
    auto s = line(7, {10, 10}, 6, {0, 6},
        {{1, 2, 1, -1}, {2, 5, 1, -1}});
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
    WeightTuningCase train{1, s, {AgentKind::Patrol}, {}};
    WeightTuningCase valid{2, s, {AgentKind::Patrol}, {}};
    WeightTuningConfig tuning;
    tuning.candidates = 2;
    tuning.beam = cfg;
    const auto first = tuneWeights({train}, {valid}, {}, tuning);
    const auto second = tuneWeights({train}, {valid}, {}, tuning);
    assert(first.evaluatedCandidates == 2);
    assert(first.validationAfter >= first.validationBefore);
    assert(first.trainingAfter == second.trainingAfter);
    assert(first.validationAfter == second.validationAfter);
    bool rejected = false;
    try { (void)tuneWeights({train}, {train}, {}, tuning); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    AgentSelectorConfig selection;
    // このテスト盤面はエージェントが1台だけなので、巡回車を残すため
    // 補給車0台を明示的に許可する。
    selection.minSupplyCount = 0;
    selection.maxSupplyCount = 0;
    selection.trialCandidateLimit = 1;
    selection.trialScenarios = {TrialTraffic(2, std::vector<TrafficStatus>(3, TrafficStatus::Smooth)),
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
    auto multi = line(7, {4, 5}, 6, {0, 3, 6},
        {{1, 0, 1, -1}, {2, 2, 1, -1}, {3, 6, 1, -1}});
    multi.field.cells[1].type = CellType::Road;
    AgentSelectorConfig fair;
    fair.maxSupplyCount = 1;
    fair.trialCandidateLimit = 3;
    fair.trialBeamWidth = 8;
    fair.trialJointBeamWidth = 32;
    fair.trialScenarios = {
        TrialTraffic(2, std::vector<TrafficStatus>(7, TrafficStatus::Smooth)),
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
        for (std::size_t i = 0; i < total.size(); ++i) total[i] += score[i];
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

    auto roads = line(16, {4, 4}, 8, {0}, {{1, 15, 1, -1}});
    for (auto& cell : roads.field.cells) cell.type = CellType::Road;
    roads.field.cells[0].type = CellType::Plain;
    const auto scenarios = makeTrialScenarios(roads);
    assert(scenarios.size() == 3);
    assert(scenarios == makeTrialScenarios(roads));
    for (const auto& scenario : scenarios) {
        assert(std::all_of(scenario[0].begin(), scenario[0].end(),
            [](TrafficStatus traffic) { return traffic == TrafficStatus::Smooth; }));
        assert(scenario[1][0] == TrafficStatus::Smooth);
    }
    assert(scenarios[0][1] != scenarios[1][1]);
    assert(scenarios[1][1] != scenarios[2][1]);
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

}  // namespace

int main() {
    testDialAgainstHeap();
    testFlowAgainstEnumeration();
    testSearchEquivalence();
    testFuelTimeTradeoff();
    testRendezvousObstaclesAndMultipleSupplies();
    testMultipleRefuelsInOneDay();
    testCoordinatedPlanningAndCache();
    testDistanceCache();
    testDetourRendezvousAndTwoSpots();
    testTrialsAndTuning();
    testConsoleReport();
}
