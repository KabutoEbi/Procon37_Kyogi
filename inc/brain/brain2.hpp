#pragma once

#include "brain1.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <vector>

namespace benzen {

namespace brain2_detail {

inline double annealingTemperature(
    const UdonAlnsConfig& config, std::size_t attempt,
    std::size_t attemptLimit) {
    const double start = std::max(config.initialTemperature, 1e-9);
    const double finish = std::clamp(config.finalTemperature, 1e-9, start);
    if (attemptLimit <= 1) return finish;
    const double progress = std::clamp(
        static_cast<double>(attempt) /
            static_cast<double>(attemptLimit - 1),
        0.0, 1.0);
    return start * std::pow(finish / start, progress);
}

inline double annealingAcceptanceProbability(double energyDelta, double temperature) {
    if (energyDelta >= 0.0) return 1.0;
    if (!(temperature > 0.0)) return 0.0;
    return std::exp(energyDelta / temperature);
}

inline bool annealingHasStagnated(
    const UdonAlnsConfig& config, std::size_t annealingAttempts,
    std::size_t attemptsSinceBest,
    const std::array<std::size_t, 7>& neighborhoodAttempts) {
    return config.stopOnStagnation && config.stagnationAttempts > 0 &&
        annealingAttempts >= config.minimumAnnealingAttempts &&
        attemptsSinceBest >= config.stagnationAttempts &&
        std::all_of(neighborhoodAttempts.begin(), neighborhoodAttempts.end(),
            [&](std::size_t attempts) {
                return attempts >= config.minimumAttemptsPerNeighborhood;
            });
}

inline std::uint64_t nextAnnealingRandom(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline double annealingUnitRandom(std::uint64_t& state) {
    return static_cast<double>(nextAnnealingRandom(state) >> 11) *
        (1.0 / 9007199254740992.0);
}

inline auto officialScore(const GameState& state) {
    return std::make_tuple(
        countBits(state.score.matchBrands),
        countBits(state.dayBrands),
        state.score.totalUdon);
}

using SpotRoutes = std::array<std::vector<std::size_t>, kMaxAgents>;

inline bool hasDuplicateVisits(const std::vector<std::size_t>& route) {
    SpotMask seen = 0;
    for (const std::size_t spot : route) {
        if (spot >= kMaxSpots || (seen & spotBit(spot))) return true;
        seen |= spotBit(spot);
    }
    return false;
}

// Recent arena leaders almost always complete both brand objectives; their
// remaining separation is only a few udon.  Spend more proposals on insertion,
// 2-opt and inter-patrol reassignment than on destructive deletion.
inline int chooseAnnealingOperation(std::uint64_t& randomState) {
    constexpr std::array<unsigned, 7> weights{{28, 4, 20, 12, 15, 12, 9}};
    constexpr unsigned total = 100;
    unsigned draw = static_cast<unsigned>(
        nextAnnealingRandom(randomState) % total);
    for (std::size_t operation = 0; operation < weights.size(); ++operation) {
        if (draw < weights[operation]) return static_cast<int>(operation);
        draw -= weights[operation];
    }
    return 0;
}

inline std::size_t chooseStockWeightedSpot(
    const MatchSetting& setting, const SpotRoutes& routes,
    std::uint64_t& randomState,
    const GameState* dayStart = nullptr,
    bool quantityAware = false) {
    std::array<unsigned, kMaxSpots> weights{};
    unsigned total = 0;
    for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot) {
        unsigned assigned = 0;
        bool brandAssigned = false;
        const int brand = setting.field.spots[spot].brandIndex;
        for (const auto& route : routes) {
            for (const std::size_t visit : route) {
                assigned += visit == spot;
                if (visit < setting.field.spots.size() &&
                    setting.field.spots[visit].brandIndex == brand) {
                    brandAssigned = true;
                }
            }
        }
        const unsigned stock = static_cast<unsigned>(std::max(0,
            dayStart && spot < dayStart->remainingStocks.size()
                ? dayStart->remainingStocks[spot]
                : setting.field.spots[spot].maxStock));
        const unsigned freeCopies = stock > assigned ? stock - assigned : 0;
        weights[spot] = quantityAware
            ? (freeCopies == 0 ? 0
                : 1 + 4 * freeCopies + (brandAssigned ? 0 : 10))
            : 1 + 3 * freeCopies + (brandAssigned ? 0 : 6);
        total += weights[spot];
    }
    if (total == 0) return setting.field.spots.size();
    unsigned draw = static_cast<unsigned>(
        nextAnnealingRandom(randomState) % total);
    for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot) {
        if (draw < weights[spot]) return spot;
        draw -= weights[spot];
    }
    return setting.field.spots.size() - 1;
}

inline std::int32_t routeTravelSteps(
    const MatchSetting& setting, const MatchState& matchState,
    CellPos start, const std::vector<std::size_t>& route, CellPos endAnchor,
    std::vector<DijkstraTable>& cache, std::vector<bool>& cached) {
    std::int64_t total = 0;
    CellPos from = start;
    const auto addLeg = [&](CellPos to, CellPos& current,
                            std::int64_t& cost) {
        const auto index = static_cast<std::size_t>(current);
        if (!cached[index]) {
            cache[index] = Brain0::dijkstra(setting, matchState, current);
            cached[index] = true;
        }
        const auto steps = cache[index].distances[static_cast<std::size_t>(to)];
        if (steps >= kImpassableCost) return false;
        cost += steps;
        current = to;
        return cost < kImpassableCost;
    };
    for (const std::size_t spot : route) {
        if (spot >= setting.field.spots.size() ||
            !addLeg(setting.field.spots[spot].pos, from, total)) {
            return kImpassableCost;
        }
    }
    if (endAnchor != kInvalidCell && !addLeg(endAnchor, from, total))
        return kImpassableCost;
    return static_cast<std::int32_t>(std::min<std::int64_t>(
        total, kImpassableCost));
}

inline std::size_t bestInsertionPosition(
    const MatchSetting& setting, const MatchState& matchState,
    CellPos start, const std::vector<std::size_t>& route,
    const std::vector<std::size_t>& block, CellPos endAnchor,
    std::vector<DijkstraTable>& cache, std::vector<bool>& cached,
    std::uint64_t& randomState) {
    std::size_t best = 0;
    std::int32_t bestCost = kImpassableCost;
    std::size_t ties = 0;
    for (std::size_t at = 0; at <= route.size(); ++at) {
        auto candidate = route;
        candidate.insert(candidate.begin() + static_cast<std::ptrdiff_t>(at),
                         block.begin(), block.end());
        const auto cost = routeTravelSteps(setting, matchState, start,
            candidate, endAnchor, cache, cached);
        if (cost < bestCost) {
            bestCost = cost;
            best = at;
            ties = 1;
        } else if (cost == bestCost &&
                   nextAnnealingRandom(randomState) % ++ties == 0) {
            best = at;
        }
    }
    return best;
}

// The official score is kept lexicographically dominant.  These diagnostics
// refine equal/near-equal tours using values obtained from the same simulator
// transition order as the judge.
struct ExtendedTourEvaluation {
    int matchBrands = 0;
    int dayBrands = 0;
    int totalUdon = 0;
    int remainingPatrolFuel = 0;
    int minimumPatrolFuel = 0;
    int lowFuelAgentSteps = 0;
    int strandedAgentSteps = 0;
    int congestionDelaySteps = 0;
    int congestedRoadExposureSteps = 0;
    int refuelEvents = 0;
    int deliveredFuel = 0;
    int servedPatrols = 0;
    int patrolMoveSteps = 0;
    int acquisitionStepSum = 0;
    double annealingEnergy = 0.0;
};

inline ExtendedTourEvaluation evaluateExtendedTour(
    const MatchSetting& setting, const MatchState& matchState,
    ScoreState previousScore, const DailyPlan& plan,
    const SimulationResult& replay, bool quantityAware = true) {
    ExtendedTourEvaluation value;
    if (!replay.valid) return value;
    value.matchBrands = countBits(replay.state.score.matchBrands);
    value.dayBrands = countBits(replay.state.dayBrands);
    value.totalUdon = replay.state.score.totalUdon;

    int patrolCount = 0;
    value.minimumPatrolFuel = std::max(0, setting.fuelLimit);
    for (AgentIndex agent = 0; agent < replay.state.agentCount; ++agent) {
        if (!replay.state.agents[agent].isPatrol()) continue;
        ++patrolCount;
        value.remainingPatrolFuel += replay.state.agents[agent].fuel;
        value.minimumPatrolFuel = std::min(
            value.minimumPatrolFuel, replay.state.agents[agent].fuel);
    }

    const auto trace = detail::repairTrace(setting, matchState, previousScore,
                                            plan, [] { return false; });
    const int lowFuelThreshold = std::max(1, setting.fuelLimit / 4);
    for (const auto& checkpoint : trace.checkpoints) {
        for (AgentIndex agent = 0; agent < checkpoint.state.agentCount; ++agent) {
            const Agent& patrol = checkpoint.state.agents[agent];
            if (!patrol.isPatrol()) continue;
            value.minimumPatrolFuel = std::min(
                value.minimumPatrolFuel, patrol.fuel);
            if (patrol.fuel <= lowFuelThreshold) ++value.lowFuelAgentSteps;

            int cheapestDeparture = kImpassableCost;
            for (int direction = 0; direction < 6; ++direction) {
                const CellPos to = setting.field.neighbor(
                    patrol.pos, static_cast<Direction>(direction));
                if (setting.field.contains(to) &&
                    setting.field.at(to).isPassable()) {
                    cheapestDeparture = std::min(cheapestDeparture,
                        moveFuelCost(setting, AgentKind::Patrol, patrol.pos));
                }
            }
            if (cheapestDeparture < kImpassableCost &&
                patrol.fuel < cheapestDeparture) {
                ++value.strandedAgentSteps;
            }
        }
    }
    if (!trace.checkpoints.empty()) {
        std::array<SpotMask, kMaxAgents> seen{};
        for (AgentIndex agent = 0; agent < replay.state.agentCount; ++agent)
            seen[agent] = trace.checkpoints.front().state.acquiredSpots[agent];
        for (std::size_t step = 1; step < trace.checkpoints.size(); ++step) {
            for (AgentIndex agent = 0; agent < replay.state.agentCount; ++agent) {
                const auto acquired =
                    trace.checkpoints[step].state.acquiredSpots[agent];
                value.acquisitionStepSum += static_cast<int>(step) *
                    countBits(acquired & ~seen[agent]);
                seen[agent] |= acquired;
            }
        }
        for (AgentIndex agent = 0; agent < replay.state.agentCount; ++agent) {
            value.acquisitionStepSum += replay.state.dayStepLimit *
                countBits(replay.state.acquiredSpots[agent] & ~seen[agent]);
        }
    }

    // Sum only the delay caused by traffic. Plain/mountain base travel time is
    // not called congestion. Positions are advanced action-by-action exactly
    // as the validated plan does.
    for (AgentIndex agent = 0; agent < matchState.agents.size(); ++agent) {
        CellPos pos = matchState.agents[agent].pos;
        for (const int action : plan[agent]) {
            if (!isMoveAction(action)) continue;
            const Cell& from = setting.field.at(pos);
            value.patrolMoveSteps += matchState.agents[agent].isPatrol()
                ? moveStepCost(setting, matchState, pos) : 0;
            value.congestionDelaySteps += std::max(0,
                moveStepCost(setting, matchState, pos) -
                    from.moveSteps(TrafficStatus::Smooth));
            pos = setting.field.neighbor(
                pos, static_cast<Direction>(action));
        }
    }
    for (std::size_t pos = 0; pos < replay.roadStaySteps.size(); ++pos) {
        if (matchState.trafficAt(static_cast<CellPos>(pos)) !=
            TrafficStatus::Smooth) {
            value.congestedRoadExposureSteps += replay.roadStaySteps[pos];
        }
    }

    std::uint32_t servedPatrolMask = 0;
    for (AgentIndex agent = 0; agent < replay.state.agentCount; ++agent) {
        const auto& usage = replay.supplyUtilization[agent];
        value.refuelEvents += usage.refuelEvents;
        value.deliveredFuel += usage.deliveredFuel;
        servedPatrolMask |= usage.servedPatrolMask;
    }
    value.servedPatrols = countBits(servedPatrolMask);

    const double fuelCapacity = std::max(1, patrolCount * setting.fuelLimit);
    const double observations = std::max(1,
        patrolCount * (replay.state.dayStepLimit + 1));
    const double travelCapacity = std::max(1,
        patrolCount * replay.state.dayStepLimit);
    const double reserve = value.remainingPatrolFuel / fuelCapacity;
    const double minimum = patrolCount == 0 ? 0.0 :
        static_cast<double>(value.minimumPatrolFuel) /
            std::max(1, setting.fuelLimit);
    const double lowFuel = value.lowFuelAgentSteps / observations;
    const double stranded = value.strandedAgentSteps / observations;
    const double congestion = value.congestionDelaySteps / travelCapacity;
    const double delivered = value.deliveredFuel / fuelCapacity;
    const double served = patrolCount == 0 ? 0.0 :
        static_cast<double>(value.servedPatrols) / patrolCount;
    const double moveLoad = value.patrolMoveSteps / travelCapacity;
    const double acquisitionDelay = value.acquisitionStepSum /
        std::max(1.0, static_cast<double>(replay.state.dayStepLimit) *
            std::max(1, replay.state.dayUdon));
    value.annealingEnergy = value.totalUdon +
        0.10 * reserve + 0.04 * minimum - 0.10 * lowFuel -
        0.18 * stranded - 0.10 * congestion +
        0.04 * delivered + 0.04 * served;
    if (quantityAware) {
        value.annealingEnergy -= 0.08 * moveLoad + 0.06 * acquisitionDelay;
    }
    return value;
}

inline UdonAlnsResult improveDayUdonAnnealed(
    const MatchSetting& setting, const MatchState& matchState,
    ScoreState previousScore, const DailyPlan& incumbent,
    const UdonAlnsConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    if (!config.simulatedAnnealing) {
        return improveDayUdon(setting, matchState, previousScore,
            incumbent, config, deadline);
    }

    UdonAlnsResult result;
    result.plan = incumbent;
    auto checked = simulateDay(
        setting, matchState, incumbent, previousScore);
    if (!checked.valid) return result;
    result.finalState = checked.state;

    // First run a true large-neighborhood phase: free two patrol suffixes
    // (three on 24x24/32x32) and, in alternating operators, the nearest supply
    // suffix.  The exact joint simulator decides rendezvous, stock ownership
    // and legality.  SA then starts from this coordinated plan instead of
    // freezing the original supply schedule immediately.
    DailyPlan annealingIncumbent = incumbent;
    const auto jointNow = Clock::now();
    const bool enoughJointTime = deadline == Clock::time_point::max() ||
        (jointNow < deadline &&
         deadline - jointNow >= std::chrono::milliseconds(1500));
    if (config.jointDestroyRepair && enoughJointTime) {
        auto jointConfig = config;
        jointConfig.simulatedAnnealing = false;
        if (setting.field.width >= 24 || setting.field.height >= 24) {
            jointConfig.maxPatrols = std::max<std::size_t>(
                jointConfig.maxPatrols, 3);
        }
        auto jointDeadline = deadline;
        if (deadline != Clock::time_point::max()) {
            const auto now = Clock::now();
            jointDeadline = now + std::max(Clock::duration::zero(),
                deadline - now) * 35 / 100;
        }
        auto joint = improveDayUdon(setting, matchState, previousScore,
            incumbent, jointConfig, jointDeadline);
        const std::size_t jointAttempts = joint.attempts;
        const std::size_t jointExpanded = joint.expanded;
        const std::size_t jointImprovements = joint.improvements;
        const auto replay = simulateDay(
            setting, matchState, joint.plan, previousScore);
        if (replay.valid &&
            officialScore(replay.state) >= officialScore(checked.state)) {
            result = std::move(joint);
            result.jointDestroyAttempts += jointAttempts;
            result.jointDestroyExpanded += jointExpanded;
            result.jointDestroyImprovements += jointImprovements;
            result.interrupted = false;
            annealingIncumbent = result.plan;
            checked = replay;
            result.finalState = checked.state;
        }
    }
    const GameState initialState = checked.state;
    const bool finalDay = matchState.day >= 0 &&
        static_cast<std::size_t>(matchState.day + 1) >= setting.dayCount();

    std::vector<AgentIndex> patrols;
    for (AgentIndex agent = 0; agent < matchState.agents.size(); ++agent) {
        if (matchState.agents[agent].isPatrol()) patrols.push_back(agent);
    }
    if (patrols.empty() || setting.field.spots.empty() || !config.maxAttempts ||
        !config.nodesPerAttempt || Clock::now() >= deadline) {
        result.interrupted = Clock::now() >= deadline;
        return result;
    }

    const auto stopped = [&] { return Clock::now() >= deadline; };
    auto supplyTrace = detail::repairTrace(
        setting, matchState, previousScore, annealingIncumbent, stopped);
    if (supplyTrace.checkpoints.empty()) return result;
    if (supplyTrace.checkpoints.size() <=
        static_cast<std::size_t>(initialState.dayStepLimit)) {
        supplyTrace.checkpoints.push_back({initialState, {}});
    }
    GameState dayStart;
    if (!dayStart.initialize(setting, matchState, previousScore)) return result;
    const detail::RepairCheckpoint startCheckpoint{dayStart, {}};
    std::vector<DijkstraTable> pathCache(setting.field.cellCount());
    std::vector<bool> pathCached(setting.field.cellCount(), false);
    const auto rebuildRoutes = [&](const SpotRoutes& routes,
                                   DailyPlan& plan,
                                   SimulationResult& replay) {
        plan = annealingIncumbent;
        for (const AgentIndex agent : patrols) {
            plan[agent].clear();
            const CellPos anchor = finalDay || config.allowFuturePositionChange
                ? kInvalidCell : initialState.agents[agent].pos;
            if (!detail::rebuildSpotSuffix(setting, matchState,
                    startCheckpoint, agent, routes[agent], anchor,
                    plan[agent], pathCache, pathCached, &supplyTrace)) {
                return false;
            }
        }
        replay = simulateDay(
            setting, matchState, plan, previousScore);
        if (!replay.valid) return false;
        return true;
    };

    std::vector<DijkstraTable> futureCache(setting.field.cellCount());
    std::vector<bool> futureCached(setting.field.cellCount(), false);
    const auto initialFuture = !finalDay && config.allowFuturePositionChange
        ? detail::futureRoutePotential(setting, matchState, initialState,
                                      futureCache, futureCached)
        : detail::FutureRoutePotential{};
    const auto preservesFuture = [&](const GameState& candidate) {
        if (finalDay || !config.preserveIntermediateState) return true;
        if (candidate.score.matchBrands != initialState.score.matchBrands ||
            candidate.dayBrands != initialState.dayBrands ||
            candidate.agentCount != initialState.agentCount) {
            return false;
        }
        if (!config.allowFuturePositionChange) {
            for (AgentIndex agent = 0; agent < candidate.agentCount; ++agent) {
                if (candidate.agents[agent].pos != initialState.agents[agent].pos ||
                    candidate.agents[agent].fuel != initialState.agents[agent].fuel) {
                    return false;
                }
            }
            return true;
        }
        const auto future = detail::futureRoutePotential(
            setting, matchState, candidate, futureCache, futureCached);
        const int extra = std::max(0,
            candidate.score.totalUdon - initialState.score.totalUdon);
        return future.reachableBrands >= initialFuture.reachableBrands &&
            future.strandedPatrols <= initialFuture.strandedPatrols &&
            future.distanceSum <= initialFuture.distanceSum + 16 + 8 * extra &&
            future.patrolFuel >= initialFuture.patrolFuel - 20 - 30 * extra;
    };

    // Construct a fresh greedy assignment. Supplies keep the incumbent time
    // table, while patrols start empty and repeatedly append the single spot
    // that gives the best official replay score.
    SpotRoutes greedyRoutes;
    DailyPlan greedyPlan = annealingIncumbent;
    for (const AgentIndex agent : patrols)
        greedyPlan[agent] = AgentPlan{-initialState.dayStepLimit};
    auto greedyReplay = simulateDay(
        setting, matchState, greedyPlan, previousScore);
    const bool greedyStartValid = greedyReplay.valid;
    GameState greedyState = greedyReplay.valid ? greedyReplay.state : initialState;
    if (!greedyReplay.valid) greedyPlan = annealingIncumbent;
    const auto greedyDeadline = deadline == Clock::time_point::max()
        ? deadline
        : Clock::now() + std::max(Clock::duration::zero(),
              deadline - Clock::now()) / 4;
    while (Clock::now() < greedyDeadline) {
        bool found = false;
        SpotRoutes bestRoutes;
        DailyPlan bestPlan;
        SimulationResult bestReplay;
        ExtendedTourEvaluation bestEvaluation;
        for (const AgentIndex agent : patrols) {
            for (std::size_t spot = 0;
                 spot < setting.field.spots.size() &&
                 Clock::now() < greedyDeadline; ++spot) {
                if (std::find(greedyRoutes[agent].begin(),
                              greedyRoutes[agent].end(), spot) !=
                    greedyRoutes[agent].end()) continue;
                auto trialRoutes = greedyRoutes;
                trialRoutes[agent].push_back(spot);
                DailyPlan trialPlan;
                SimulationResult trialReplay;
                if (!rebuildRoutes(trialRoutes, trialPlan, trialReplay)) continue;
                ++result.attempts;
                ++result.greedyAttempts;
                const auto trialEvaluation = evaluateExtendedTour(
                    setting, matchState, previousScore, trialPlan, trialReplay,
                    config.quantityAwareNeighborhoods);
                if (officialScore(trialReplay.state) <= officialScore(greedyState) ||
                    (found && std::make_tuple(
                         officialScore(trialReplay.state),
                         trialEvaluation.annealingEnergy) <=
                     std::make_tuple(officialScore(bestReplay.state),
                         bestEvaluation.annealingEnergy))) {
                    continue;
                }
                found = true;
                bestRoutes = std::move(trialRoutes);
                bestPlan = std::move(trialPlan);
                bestReplay = std::move(trialReplay);
                bestEvaluation = trialEvaluation;
            }
        }
        if (!found) break;
        greedyRoutes = std::move(bestRoutes);
        greedyPlan = std::move(bestPlan);
        greedyReplay = std::move(bestReplay);
        greedyState = greedyReplay.state;
    }

    // If the fresh greedy construction missed a brand already secured by the
    // incumbent, recover its explicit visit order. The incumbent remains the
    // global-best guard in either case.
    if (!greedyStartValid || countBits(greedyState.score.matchBrands) <
            countBits(initialState.score.matchBrands) ||
        countBits(greedyState.dayBrands) < countBits(initialState.dayBrands) ||
        !preservesFuture(greedyState)) {
        greedyPlan = annealingIncumbent;
        greedyReplay = checked;
        greedyState = initialState;
        const auto incumbentTrace = detail::repairTrace(
            setting, matchState, previousScore, annealingIncumbent, stopped);
        if (!incumbentTrace.checkpoints.empty()) {
            for (const AgentIndex agent : patrols) {
                greedyRoutes[agent] = detail::acquiredSpotSequence(
                    incumbentTrace, initialState, agent, 0);
            }
        }
    }

    DailyPlan currentPlan = greedyPlan;
    GameState currentState = greedyState;
    auto currentEvaluation = evaluateExtendedTour(
        setting, matchState, previousScore, currentPlan, greedyReplay,
        config.quantityAwareNeighborhoods);
    auto bestEvaluation = evaluateExtendedTour(
        setting, matchState, previousScore, result.plan, checked,
        config.quantityAwareNeighborhoods);
    if (officialScore(greedyState) > officialScore(result.finalState)) {
        result.plan = greedyPlan;
        result.finalState = greedyState;
        bestEvaluation = currentEvaluation;
        ++result.improvements;
    }

    std::uint64_t randomState = config.randomSeed ^
        (static_cast<std::uint64_t>(
             static_cast<std::uint32_t>(matchState.day)) << 32) ^
        static_cast<std::uint32_t>(initialState.score.totalUdon);
    const bool finiteDeadline = deadline != Clock::time_point::max();
    const std::size_t annealingLimit = finiteDeadline
        ? std::max<std::size_t>(4096,
              (config.maxAttempts + config.improvementBonusAttempts) * 256)
        : std::max<std::size_t>(256,
              (config.maxAttempts + config.improvementBonusAttempts) * 16);
    std::size_t annealingAttempts = 0;
    std::size_t attemptsAtBest = 0;
    for (std::size_t iteration = 0;
         iteration < annealingLimit && Clock::now() < deadline; ++iteration) {
        auto trialRoutes = greedyRoutes;
        const auto randomPatrol = [&] {
            return patrols[nextAnnealingRandom(randomState) % patrols.size()];
        };
        const int operation = config.arenaGuidedNeighborhoods
            ? chooseAnnealingOperation(randomState)
            : static_cast<int>(nextAnnealingRandom(randomState) % 7);
        AgentIndex first = randomPatrol();
        AgentIndex second = randomPatrol();
        if (patrols.size() > 1) {
            while (second == first) second = randomPatrol();
        }
        auto& firstRoute = trialRoutes[first];
        auto& secondRoute = trialRoutes[second];
        const auto endAnchor = [&](AgentIndex agent) {
            return finalDay || config.allowFuturePositionChange
                ? kInvalidCell : initialState.agents[agent].pos;
        };
        const std::size_t randomSpot = config.stockWeightedSpotSelection
            ? chooseStockWeightedSpot(setting, trialRoutes, randomState,
                  &dayStart, config.quantityAwareNeighborhoods)
            : nextAnnealingRandom(randomState) % setting.field.spots.size();
        bool changed = false;
        if (operation == 0) { // add an assigned spot
            if (randomSpot < setting.field.spots.size() &&
                std::find(firstRoute.begin(), firstRoute.end(), randomSpot) ==
                firstRoute.end()) {
                const std::size_t at = config.quantityAwareNeighborhoods
                    ? bestInsertionPosition(setting, matchState,
                          dayStart.agents[first].pos, firstRoute, {randomSpot},
                          endAnchor(first), pathCache, pathCached, randomState)
                    : nextAnnealingRandom(randomState) % (firstRoute.size() + 1);
                firstRoute.insert(firstRoute.begin() + at, randomSpot);
                changed = true;
            }
        } else if (operation == 1 && !firstRoute.empty()) { // remove
            firstRoute.erase(firstRoute.begin() +
                nextAnnealingRandom(randomState) % firstRoute.size());
            changed = true;
        } else if (operation == 2 && firstRoute.size() >= 2) { // 2-opt
            std::size_t left = nextAnnealingRandom(randomState) % firstRoute.size();
            std::size_t right = nextAnnealingRandom(randomState) % firstRoute.size();
            if (left > right) std::swap(left, right);
            if (left != right) {
                std::reverse(firstRoute.begin() + left,
                             firstRoute.begin() + right + 1);
                changed = true;
            }
        } else if (operation == 3 && firstRoute.size() >= 2) { // relocate block
            const std::size_t from = nextAnnealingRandom(randomState) %
                firstRoute.size();
            const std::size_t length = config.quantityAwareNeighborhoods
                ? std::min<std::size_t>(1 + nextAnnealingRandom(randomState) % 2,
                      firstRoute.size() - from)
                : 1;
            std::vector<std::size_t> block(
                firstRoute.begin() + static_cast<std::ptrdiff_t>(from),
                firstRoute.begin() + static_cast<std::ptrdiff_t>(from + length));
            firstRoute.erase(
                firstRoute.begin() + static_cast<std::ptrdiff_t>(from),
                firstRoute.begin() + static_cast<std::ptrdiff_t>(from + length));
            const std::size_t at = config.quantityAwareNeighborhoods
                ? bestInsertionPosition(setting, matchState,
                      dayStart.agents[first].pos, firstRoute, block,
                      endAnchor(first), pathCache, pathCached, randomState)
                : nextAnnealingRandom(randomState) % (firstRoute.size() + 1);
            firstRoute.insert(firstRoute.begin() + static_cast<std::ptrdiff_t>(at),
                              block.begin(), block.end());
            changed = at != from;
        } else if (operation == 4 && first != second &&
                   !firstRoute.empty()) { // transfer block between patrols
            const std::size_t from = nextAnnealingRandom(randomState) %
                firstRoute.size();
            const std::size_t length = config.quantityAwareNeighborhoods
                ? std::min<std::size_t>(1 + nextAnnealingRandom(randomState) % 2,
                      firstRoute.size() - from)
                : 1;
            std::vector<std::size_t> block(
                firstRoute.begin() + static_cast<std::ptrdiff_t>(from),
                firstRoute.begin() + static_cast<std::ptrdiff_t>(from + length));
            const bool available = std::all_of(block.begin(), block.end(),
                [&](std::size_t visit) {
                    return std::find(secondRoute.begin(), secondRoute.end(), visit) ==
                        secondRoute.end();
                });
            if (available) {
                firstRoute.erase(
                    firstRoute.begin() + static_cast<std::ptrdiff_t>(from),
                    firstRoute.begin() + static_cast<std::ptrdiff_t>(from + length));
                const std::size_t at = config.quantityAwareNeighborhoods
                    ? bestInsertionPosition(setting, matchState,
                          dayStart.agents[second].pos, secondRoute, block,
                          endAnchor(second), pathCache, pathCached, randomState)
                    : nextAnnealingRandom(randomState) % (secondRoute.size() + 1);
                secondRoute.insert(
                    secondRoute.begin() + static_cast<std::ptrdiff_t>(at),
                    block.begin(), block.end());
                changed = true;
            }
        } else if (operation == 5 && first != second &&
                   !firstRoute.empty() && !secondRoute.empty()) { // swap
            const std::size_t x = nextAnnealingRandom(randomState) %
                firstRoute.size();
            const std::size_t y = nextAnnealingRandom(randomState) %
                secondRoute.size();
            std::swap(firstRoute[x], secondRoute[y]);
            changed = true;
        } else if (operation == 6 && randomSpot < setting.field.spots.size() &&
                   !firstRoute.empty() &&
                   std::find(firstRoute.begin(), firstRoute.end(), randomSpot) ==
                       firstRoute.end()) { // replace
            const std::size_t replaced = nextAnnealingRandom(randomState) %
                firstRoute.size();
            firstRoute.erase(firstRoute.begin() +
                static_cast<std::ptrdiff_t>(replaced));
            const std::size_t at = config.quantityAwareNeighborhoods
                ? bestInsertionPosition(setting, matchState,
                      dayStart.agents[first].pos, firstRoute, {randomSpot},
                      endAnchor(first), pathCache, pathCached, randomState)
                : std::min(replaced, firstRoute.size());
            firstRoute.insert(firstRoute.begin() +
                static_cast<std::ptrdiff_t>(at), randomSpot);
            changed = true;
        }
        if (!changed || hasDuplicateVisits(firstRoute) ||
            hasDuplicateVisits(secondRoute)) continue;
        ++result.neighborhoodAttempts[static_cast<std::size_t>(operation)];
        ++annealingAttempts;

        DailyPlan trialPlan;
        SimulationResult trialReplay;
        if (!rebuildRoutes(trialRoutes, trialPlan, trialReplay) ||
            !preservesFuture(trialReplay.state)) {
            if (annealingHasStagnated(config, annealingAttempts,
                    annealingAttempts - attemptsAtBest,
                    result.neighborhoodAttempts)) {
                result.stagnationStopped = true;
                break;
            }
            continue;
        }
        const GameState& trialState = trialReplay.state;
        ++result.attempts;
        ++result.expanded;
        if (countBits(trialState.score.matchBrands) <
                countBits(result.finalState.score.matchBrands) ||
            countBits(trialState.dayBrands) <
                countBits(result.finalState.dayBrands)) {
            if (annealingHasStagnated(config, annealingAttempts,
                    annealingAttempts - attemptsAtBest,
                    result.neighborhoodAttempts)) {
                result.stagnationStopped = true;
                break;
            }
            continue;
        }

        const auto kickedScore = officialScore(trialState);
        const auto currentScore = officialScore(currentState);
        const auto trialEvaluation = evaluateExtendedTour(
            setting, matchState, previousScore, trialPlan, trialReplay,
            config.quantityAwareNeighborhoods);
        const bool equalBrandCounts =
            std::get<0>(kickedScore) == std::get<0>(currentScore) &&
            std::get<1>(kickedScore) == std::get<1>(currentScore);
        const double energyDelta = trialEvaluation.annealingEnergy -
            currentEvaluation.annealingEnergy;
        bool accepted = kickedScore > currentScore ||
            (equalBrandCounts && energyDelta >= 0.0);
        bool acceptedWorse = false;
        if (equalBrandCounts && energyDelta < 0.0) {
            const double temperature = annealingTemperature(
                config, iteration, annealingLimit);
            accepted = annealingUnitRandom(randomState) <
                annealingAcceptanceProbability(energyDelta, temperature);
            acceptedWorse = accepted;
        }
        if (accepted) {
            greedyRoutes = std::move(trialRoutes);
            currentPlan = std::move(trialPlan);
            currentState = trialState;
            currentEvaluation = trialEvaluation;
            ++result.acceptedMoves;
            ++result.neighborhoodAccepted[static_cast<std::size_t>(operation)];
            if (acceptedWorse) ++result.acceptedWorseMoves;
        }
        const auto bestScore = officialScore(result.finalState);
        if (accepted && (kickedScore > bestScore ||
            (kickedScore == bestScore &&
             currentEvaluation.annealingEnergy >
                 bestEvaluation.annealingEnergy + 1e-12))) {
            result.plan = currentPlan;
            result.finalState = trialState;
            bestEvaluation = currentEvaluation;
            ++result.improvements;
            attemptsAtBest = annealingAttempts;
        }
        if (annealingHasStagnated(config, annealingAttempts,
                annealingAttempts - attemptsAtBest,
                result.neighborhoodAttempts)) {
            result.stagnationStopped = true;
            break;
        }
    }
    if (Clock::now() >= deadline) result.interrupted = true;
    return result;
}

} // namespace brain2_detail

inline BeamSearchResult brain2(
    const MatchSetting& setting, const MatchState& matchState,
    ScoreState previousScore = {},
    const BeamSearchConfig& requestedConfig = {}) {
    auto config = requestedConfig;
    if (config.useUdonAlns && config.udonAlns.simulatedAnnealing) {
        config.udonRefiner = brain2_detail::improveDayUdonAnnealed;
    }
    return brain1(setting, matchState, previousScore, config);
}

} // namespace benzen
