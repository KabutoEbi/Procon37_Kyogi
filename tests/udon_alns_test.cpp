#include "model.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <random>
#include <tuple>

using namespace benzen;

namespace {
MatchSetting line(int width, int steps, std::vector<CellPos> starts,
                  std::vector<Spot> spots) {
    MatchSetting s;
    s.field.width = width;
    s.field.height = 1;
    s.field.cells.assign(width, {CellType::Plain, -1});
    s.field.spots = std::move(spots);
    assert(s.field.rebuildSpotIndex());
    s.agentStarts = std::move(starts);
    s.daySteps = {steps};
    s.daySeconds = {60};
    s.fuelLimit = steps;
    return s;
}

MatchState stateFor(const MatchSetting& s,
                    const std::vector<AgentKind>& kinds) {
    MatchState ms;
    ms.trafficByPos.assign(s.field.cellCount(), TrafficStatus::Smooth);
    for (AgentIndex a = 0; a < kinds.size(); ++a)
        ms.agents.push_back({a, kinds[a], s.agentStarts[a], s.fuelLimit});
    return ms;
}

void checkSameBrandExtraUdon() {
    const auto s = line(4, 8, {0}, {{0, 1, 1, -1}, {0, 3, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol});
    const DailyPlan incumbent{{2, -6}};
    const auto before = simulateDay(s, ms, incumbent);
    assert(before.valid && before.state.dayUdon == 1 && before.state.dayBrands == 1);

    UdonAlnsConfig cfg;
    cfg.maxAttempts = 12;
    cfg.nodesPerAttempt = 2000;
    cfg.initialTemperature = cfg.finalTemperature = 1e6;
    const auto improved = brain2_detail::improveDayUdonAnnealed(
        s, ms, {}, incumbent, cfg,
        std::chrono::steady_clock::time_point::max());
    assert(improved.improvements > 0);
    assert(improved.acceptedWorseMoves > 0);
    assert(improved.greedyAttempts > 0);
    assert(improved.jointDestroyAttempts > 0);
    assert(improved.neighborhoodAttempts[0] > 0); // insertion
    assert(improved.neighborhoodAttempts[2] > 0); // 2-opt
    assert(improved.attempts > cfg.maxAttempts);
    const std::size_t annealingLimit = std::max<std::size_t>(
        256, (cfg.maxAttempts + cfg.improvementBonusAttempts) * 16);
    assert(improved.attempts <= annealingLimit +
        s.field.spots.size() * s.field.spots.size());
    assert(improved.finalState.dayBrands == before.state.dayBrands);
    assert(improved.finalState.score.matchBrands == before.state.score.matchBrands);
    assert(improved.finalState.dayUdon == 2);
    const auto replay = simulateDay(s, ms, improved.plan);
    assert(replay.valid && replay.state == improved.finalState);
}

void checkBrainMaximizesBrandsBeforeUdon() {
    const auto s = line(4, 8, {0}, {{0, 1, 1, -1}, {0, 3, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol});
    BeamSearchConfig cfg;
    cfg.beamWidth = cfg.jointBeamWidth = cfg.maxActionsPerAgent = 1;
    cfg.macroTargetLimit = 1;
    cfg.adaptiveBeamWidth = cfg.adaptiveActionLimit = cfg.autoTuneWeights = false;
    cfg.coordinatedAssignments = cfg.twoSpotLookahead = false;
    cfg.comparePeriodicReplanPlans = cfg.iterativeDeepening = false;
    cfg.useStateDeadline = false;
    cfg.useBrandRepair = false;
    cfg.exactBrands = true;
    cfg.udonAlns.maxAttempts = 12;
    cfg.udonAlns.nodesPerAttempt = 2000;

    const auto result = brain1(s, ms, {}, cfg);
    assert(result.success && result.brandOptimal);
    assert(countBits(result.finalState.dayBrands) == 1);
    assert(result.finalState.dayUdon == 2);
    assert(result.udonAlnsAttempts > 0);
    const auto replay = simulateDay(s, ms, result.plan);
    assert(replay.valid && replay.state == result.finalState);

    cfg.udonAlns.initialTemperature = 1e6;
    cfg.udonAlns.finalTemperature = 1e6;
    const auto annealed = brain2(s, ms, {}, cfg);
    assert(annealed.success);
    assert(annealed.udonAlnsAcceptedWorseMoves > 0);
    assert(brain2_detail::officialScore(annealed.finalState) >=
           brain2_detail::officialScore(result.finalState));
    const auto annealedReplay = simulateDay(s, ms, annealed.plan);
    assert(annealedReplay.valid && annealedReplay.state == annealed.finalState);
}

void checkIntermediateDayPreservesFutureState() {
    auto s = line(3, 6, {1, 0}, {{0, 0, 1, -1}, {0, 2, 1, -1}});
    s.daySteps.push_back(6);
    s.daySeconds.push_back(60);
    auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Supply});
    const DailyPlan incumbent{{5, -4}, {-6}};
    const auto before = simulateDay(s, ms, incumbent);
    assert(before.valid && before.state.dayUdon == 1);

    UdonAlnsConfig cfg;
    cfg.maxAttempts = 20;
    cfg.nodesPerAttempt = 5000;
    cfg.maxPatrols = 1;
    cfg.allowFuturePositionChange = false;
    const auto improved = improveDayUdon(s, ms, {}, incumbent, cfg,
        std::chrono::steady_clock::time_point::max());
    assert(improved.finalState.dayUdon > before.state.dayUdon);
    assert(improved.improvements > 0);
    assert(improved.finalState.score.matchBrands == before.state.score.matchBrands);
    assert(improved.finalState.dayBrands == before.state.dayBrands);
    for (AgentIndex a = 0; a < before.state.agentCount; ++a) {
        assert(improved.finalState.agents[a].pos == before.state.agents[a].pos);
        assert(improved.finalState.agents[a].fuel == before.state.agents[a].fuel);
    }
    const auto replay = simulateDay(s, ms, improved.plan);
    assert(replay.valid && replay.state == improved.finalState);
}

void checkRandomValidityAndPreservedBrandCounts() {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        CreateConfig create;
        create.width = create.height = 8;
        create.agentCount = 4;
        create.spotCount = 8;
        create.brandCount = 4;
        create.seed = seed;
        auto s = ProblemCreator::generate(create).setting;
        s.daySteps.resize(1);
        s.daySeconds.resize(1);
        s.daySteps[0] = 18;
        auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol,
                              AgentKind::Supply, AgentKind::Supply});
        for (std::size_t i = 0; i < ms.trafficByPos.size(); ++i)
            ms.trafficByPos[i] = static_cast<TrafficStatus>((i + seed) % 3);
        BeamSearchConfig beam;
        beam.refineBrands = false;
        beam.useUdonAlns = false;
        beam.beamWidth = 8;
        beam.jointBeamWidth = 16;
        beam.threadCount = 1;
        beam.useStateDeadline = beam.iterativeDeepening = false;
        beam.comparePeriodicReplanPlans = false;
        const auto incumbent = brain1(s, ms, {}, beam);
        assert(incumbent.success);

        UdonAlnsConfig cfg;
        cfg.maxAttempts = 6;
        cfg.nodesPerAttempt = 120;
        const auto result = improveDayUdon(s, ms, {}, incumbent.plan, cfg,
            std::chrono::steady_clock::time_point::max());
        const auto replay = simulateDay(s, ms, result.plan);
        assert(replay.valid && replay.state == result.finalState);
        const auto incumbentScore = std::make_tuple(
            countBits(incumbent.finalState.score.matchBrands),
            countBits(incumbent.finalState.dayBrands),
            incumbent.finalState.score.totalUdon);
        const auto resultScore = std::make_tuple(
            countBits(result.finalState.score.matchBrands),
            countBits(result.finalState.dayBrands),
            result.finalState.score.totalUdon);
        assert(resultScore >= incumbentScore);
        assert(result.attempts <=
               cfg.maxAttempts + cfg.improvementBonusAttempts);
        assert(result.expanded <=
               (cfg.maxAttempts + cfg.improvementBonusAttempts) *
                   cfg.nodesPerAttempt);
    }
}

void checkNearestSupplyUsesTrafficTravelTime() {
    auto s = line(5, 8, {2, 1, 4}, {});
    s.field.cells[1].type = CellType::Road;
    s.field.cells[3].type = CellType::Road;
    auto ms = stateFor(s, {
        AgentKind::Patrol, AgentKind::Supply, AgentKind::Supply});
    ms.trafficByPos[1] = TrafficStatus::Jammed;
    ms.trafficByPos[3] = TrafficStatus::Smooth;
    GameState state;
    assert(state.initialize(s, ms));
    std::vector<DijkstraTable> cache(s.field.cellCount());
    std::vector<bool> cached(s.field.cellCount(), false);
    // Supply 1 is geometrically adjacent but leaving its jammed road takes
    // four steps. Supply 2 reaches the patrol in three steps.
    assert(detail::nearestSupplyByTravel(
               s, ms, state, 0, cache, cached) == 2);

    auto blocked = line(3, 4, {0, 2}, {});
    blocked.field.cells[1].type = CellType::Pond;
    auto blockedState = stateFor(
        blocked, {AgentKind::Patrol, AgentKind::Supply});
    GameState blockedGame;
    assert(blockedGame.initialize(blocked, blockedState));
    cache.assign(blocked.field.cellCount(), DijkstraTable{});
    cached.assign(blocked.field.cellCount(), false);
    assert(detail::nearestSupplyByTravel(
               blocked, blockedState, blockedGame, 0, cache, cached) ==
           blockedGame.agentCount);
}

void checkAdaptiveLongSearchProfile() {
    MatchSetting setting;
    setting.field.width = setting.field.height = 16;
    BeamSearchConfig config;
    const auto shortRun = detail::adaptiveLongSearchProfile(
        setting, config, 10000, true, false);
    assert(shortRun.quantityBudgetPercent == 18);
    assert(shortRun.alnsPercentOfRefinement == 20);
    assert(shortRun.udonAlns.maxAttempts == 12);
    assert(shortRun.udonAlns.nodesPerAttempt == 800);

    setting.field.width = setting.field.height = 24;
    const auto middleFinal = detail::adaptiveLongSearchProfile(
        setting, config, 35000, true, false);
    assert(middleFinal.quantityBudgetPercent == 30);
    assert(middleFinal.alnsPercentOfRefinement == 40);
    assert(middleFinal.udonAlns.maxAttempts == 21);
    assert(middleFinal.udonAlns.nodesPerAttempt == 1350);
    assert(middleFinal.udonAlns.improvementBonusAttempts == 6);

    setting.field.width = setting.field.height = 32;
    const auto longFinal = detail::adaptiveLongSearchProfile(
        setting, config, 55000, true, false);
    assert(longFinal.quantityBudgetPercent == 35);
    assert(longFinal.alnsPercentOfRefinement == 45);
    assert(longFinal.udonAlns.maxAttempts == 28);
    assert(longFinal.udonAlns.nodesPerAttempt == 1400);
    assert(longFinal.udonAlns.improvementBonusAttempts == 8);
    const auto longIntermediate = detail::adaptiveLongSearchProfile(
        setting, config, 55000, false, false);
    assert(longIntermediate.quantityBudgetPercent == 30);
    assert(longIntermediate.alnsPercentOfRefinement == 35);

    const auto fuelRisk = detail::adaptiveLongSearchProfile(
        setting, config, 55000, true, true);
    assert(fuelRisk.quantityBudgetPercent == 15);
    assert(fuelRisk.alnsPercentOfRefinement == 20);

    config.adaptiveLongSearch = false;
    const auto disabled = detail::adaptiveLongSearchProfile(
        setting, config, 55000, true, false);
    assert(disabled.quantityBudgetPercent == 25);
    assert(disabled.alnsPercentOfRefinement == 25);
    assert(disabled.udonAlns.maxAttempts == 12);
    assert(disabled.udonAlns.nodesPerAttempt == 800);
}

void checkAnnealingScheduleAndAcceptance() {
    UdonAlnsConfig cfg;
    cfg.initialTemperature = 4.0;
    cfg.finalTemperature = 0.25;
    const double first = brain2_detail::annealingTemperature(cfg, 0, 9);
    const double middle = brain2_detail::annealingTemperature(cfg, 4, 9);
    const double last = brain2_detail::annealingTemperature(cfg, 8, 9);
    assert(std::abs(first - 4.0) < 1e-12);
    assert(std::abs(middle - 1.0) < 1e-12);
    assert(std::abs(last - 0.25) < 1e-12);
    assert(brain2_detail::annealingAcceptanceProbability(0, 1.0) == 1.0);
    assert(brain2_detail::annealingAcceptanceProbability(1, 1.0) == 1.0);
    assert(std::abs(brain2_detail::annealingAcceptanceProbability(-1, 1.0) -
                    std::exp(-1.0)) < 1e-12);
    assert(brain2_detail::annealingAcceptanceProbability(-1, 0.0) == 0.0);

    std::uint64_t firstState = 12345;
    std::uint64_t secondState = 12345;
    for (int i = 0; i < 8; ++i) {
        const double a = brain2_detail::annealingUnitRandom(firstState);
        const double b = brain2_detail::annealingUnitRandom(secondState);
        assert(a == b && a >= 0.0 && a < 1.0);
    }
}

void checkAnnealingStagnationRule() {
    UdonAlnsConfig cfg;
    cfg.stopOnStagnation = true;
    cfg.minimumAnnealingAttempts = 100;
    cfg.stagnationAttempts = 40;
    cfg.minimumAttemptsPerNeighborhood = 3;
    std::array<std::size_t, 7> covered{};
    covered.fill(3);
    assert(!brain2_detail::annealingHasStagnated(cfg, 99, 50, covered));
    assert(!brain2_detail::annealingHasStagnated(cfg, 100, 39, covered));
    auto missing = covered;
    missing[6] = 2;
    assert(!brain2_detail::annealingHasStagnated(cfg, 100, 40, missing));
    assert(brain2_detail::annealingHasStagnated(cfg, 100, 40, covered));
    cfg.stopOnStagnation = false;
    assert(!brain2_detail::annealingHasStagnated(cfg, 1000, 1000, covered));
}

void checkExtendedTourEvaluationUsesOfficialReplay() {
    auto smoothSetting = line(2, 4, {0}, {{0, 1, 1, -1}});
    for (auto& cell : smoothSetting.field.cells) cell.type = CellType::Road;
    auto smoothState = stateFor(smoothSetting, {AgentKind::Patrol});
    const DailyPlan smoothPlan{{2, -3}};
    const auto smoothReplay = simulateDay(
        smoothSetting, smoothState, smoothPlan);
    assert(smoothReplay.valid);
    const auto smooth = brain2_detail::evaluateExtendedTour(
        smoothSetting, smoothState, {}, smoothPlan, smoothReplay);
    assert(smooth.congestionDelaySteps == 0);
    assert(smooth.patrolMoveSteps == 1);
    assert(smooth.acquisitionStepSum == 1);

    auto jammedState = smoothState;
    jammedState.trafficByPos[0] = TrafficStatus::Jammed;
    const DailyPlan jammedPlan{{2}};
    const auto jammedReplay = simulateDay(
        smoothSetting, jammedState, jammedPlan);
    assert(jammedReplay.valid);
    const auto jammed = brain2_detail::evaluateExtendedTour(
        smoothSetting, jammedState, {}, jammedPlan, jammedReplay);
    assert(jammed.congestionDelaySteps == 3);
    assert(jammed.congestedRoadExposureSteps == 3);
    assert(jammed.patrolMoveSteps == 4);
    assert(jammed.acquisitionStepSum == 4);
    assert(jammed.annealingEnergy < smooth.annealingEnergy);

    auto supplySetting = line(3, 6, {0, 0}, {{0, 2, 1, -1}});
    auto supplyState = stateFor(
        supplySetting, {AgentKind::Patrol, AgentKind::Supply});
    supplyState.agents[0].fuel = 0;
    const DailyPlan rendezvousPlan{{-1, 2, 2, -1}, {-6}};
    const auto rendezvousReplay = simulateDay(
        supplySetting, supplyState, rendezvousPlan);
    assert(rendezvousReplay.valid);
    const auto rendezvous = brain2_detail::evaluateExtendedTour(
        supplySetting, supplyState, {}, rendezvousPlan, rendezvousReplay);
    assert(rendezvous.refuelEvents >= 1);
    assert(rendezvous.deliveredFuel == supplySetting.fuelLimit);
    assert(rendezvous.servedPatrols == 1);
    assert(rendezvous.minimumPatrolFuel == 0);
    assert(rendezvous.lowFuelAgentSteps >= 1);
    assert(rendezvous.strandedAgentSteps >= 1);
}

void checkArenaGuidedNeighborhoodSampling() {
    const UdonAlnsConfig defaults;
    assert(!defaults.arenaGuidedNeighborhoods);
    assert(defaults.stockWeightedSpotSelection);
    assert(defaults.quantityAwareNeighborhoods);
    const auto s = line(2, 4, {0},
        {{0, 0, 1, -1}, {0, 1, 5, -1}});
    brain2_detail::SpotRoutes routes;
    std::array<int, 2> spotDraws{};
    std::array<int, 7> operationDraws{};
    std::uint64_t randomState = 987654321;
    for (int sample = 0; sample < 10000; ++sample) {
        ++spotDraws[brain2_detail::chooseStockWeightedSpot(
            s, routes, randomState)];
        ++operationDraws[brain2_detail::chooseAnnealingOperation(
            randomState)];
    }
    assert(spotDraws[1] > spotDraws[0]);
    assert(operationDraws[0] > operationDraws[1]);
    assert(operationDraws[2] > operationDraws[1]);
    for (const int draws : operationDraws) assert(draws > 0);
}

void checkQuantityAwareSpotSelectionAndInsertion() {
    const auto s = line(5, 8, {0},
        {{0, 0, 3, -1}, {0, 2, 1, -1}, {1, 4, 4, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol});
    GameState dayStart;
    assert(dayStart.initialize(s, ms));
    dayStart.remainingStocks[0] = 0;
    brain2_detail::SpotRoutes routes;
    std::array<int, 3> draws{};
    std::uint64_t randomState = 246813579;
    for (int sample = 0; sample < 10000; ++sample) {
        const auto spot = brain2_detail::chooseStockWeightedSpot(
            s, routes, randomState, &dayStart, true);
        assert(spot < s.field.spots.size());
        ++draws[spot];
    }
    assert(draws[0] == 0);
    assert(draws[2] > draws[1]);

    std::vector<DijkstraTable> cache(s.field.cellCount());
    std::vector<bool> cached(s.field.cellCount(), false);
    std::vector<std::size_t> route{2};
    randomState = 13579;
    const auto at = brain2_detail::bestInsertionPosition(
        s, ms, s.agentStarts[0], route, {1}, kInvalidCell,
        cache, cached, randomState);
    assert(at == 0);
    route.insert(route.begin() + static_cast<std::ptrdiff_t>(at), 1);
    assert(brain2_detail::routeTravelSteps(
               s, ms, s.agentStarts[0], route, kInvalidCell,
               cache, cached) == 8);
}
}  // namespace

void checkReassignConsumedStock() {
    const auto s = line(6, 6, {2, 4},
        {{0, 0, 1, -1}, {0, 3, 1, -1}, {0, 5, 1, -1}});
    const auto ms = stateFor(s, {AgentKind::Patrol, AgentKind::Patrol});
    const DailyPlan incumbent{{2, 2, 2}, {-6}};
    const auto before = simulateDay(s, ms, incumbent);
    assert(before.valid && before.state.dayUdon == 2);
    UdonAlnsConfig cfg;
    cfg.nodesPerAttempt = 12000;
    cfg.maxAttempts = 24;
    const auto result = improveDayUdon(s, ms, {}, incumbent, cfg,
        std::chrono::steady_clock::time_point::max());
    assert(result.finalState.dayUdon == 3);
    assert(result.finalState.dayBrands == before.state.dayBrands);
    assert((result.finalState.acquiredSpots[1] & spotBit(1)) != 0);
    const auto replay = simulateDay(s, ms, result.plan);
    assert(replay.valid && replay.state == result.finalState);
}

int main() {
    checkReassignConsumedStock();
    checkSameBrandExtraUdon();
    checkBrainMaximizesBrandsBeforeUdon();
    checkIntermediateDayPreservesFutureState();
    checkRandomValidityAndPreservedBrandCounts();
    checkNearestSupplyUsesTrafficTravelTime();
    checkAdaptiveLongSearchProfile();
    checkAnnealingScheduleAndAcceptance();
    checkAnnealingStagnationRule();
    checkExtendedTourEvaluationUsesOfficialReplay();
    checkArenaGuidedNeighborhoodSampling();
    checkQuantityAwareSpotSelectionAndInsertion();
}
