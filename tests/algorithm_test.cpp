#include "model.hpp"

#include <cassert>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>

using namespace procon37;

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
    for (int test = 0; test < 40; ++test) {
        MinCostFlow flow(sink + 1);
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
    selection.trialCandidateLimit = 1;
    selection.trialScenarios = {TrialTraffic(2, std::vector<TrafficStatus>(3, TrafficStatus::Smooth)),
                                TrialTraffic(2, std::vector<TrafficStatus>(3, TrafficStatus::Jammed))};
    const auto selected = selectAgentKinds(s, selection);
    assert(selected.success && selected.trialSimulations == 2);
    assert((selected.worstTrialScore == TrialScore{2, 3, 3}));
}

void testConsoleReport() {
    const auto s = line(3, {4}, 3, {0}, {{1, 2, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol});
    const auto result = brain1(s, ms, {}, smallBeam());
    assert(result.success);

    ConsoleConfig config;
    config.traceEveryStep = true;
    std::ostringstream output;
    printConsoleReport(output, s, ms, result, {}, config);
    const std::string text = output.str();
    assert(text.find("=== procon37 solver report ===") != std::string::npos);
    assert(text.find("map 3x1") != std::string::npos);
    assert(text.find("search success=true") != std::string::npos);
    assert(text.find("macro-candidates=") != std::string::npos);
    assert(text.find("plan\n") != std::string::npos);
    assert(text.find("trace\n") != std::string::npos);
    assert(text.find("evaluation total=") != std::string::npos);
    assert(text.find("result udon=1") != std::string::npos);

    DailyPlan invalid(1);
    invalid[0] = {2};
    std::ostringstream error;
    assert(!printConsoleTrace(error, s, ms, invalid));
    assert(error.str().find("trace invalid:") != std::string::npos);
}

}  // namespace

int main() {
    testDialAgainstHeap();
    testFlowAgainstEnumeration();
    testSearchEquivalence();
    testFuelTimeTradeoff();
    testRendezvousObstaclesAndMultipleSupplies();
    testTrialsAndTuning();
    testConsoleReport();
}
