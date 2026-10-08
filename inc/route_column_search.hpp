#pragma once

#include "spot_route_lns.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

namespace benzen {

struct RouteColumnConfig {
    bool enabled = true;
    // Below this whole-day budget, legacy refiners have a better payoff.
    std::int64_t minimumBudgetMs = 5000;
    std::size_t labelWidth = 24;
    std::size_t columnsPerAgent = 32;
    std::size_t maxVisits = 14;
    std::size_t maxMasterNodes = 150000;
    // Rank labels by every spot crossed on the restored shortest path, not
    // only by the explicitly selected destination.  Replay remains the oracle.
    bool rankTransitPickups = true;
    bool supplyRendezvous = true;
    std::size_t maxSupplySchedules = 8;
    // 当日開始時点で動けない、または満タンの1割以下の巡回車へ
    // 補給車が直行する候補を、通常の合流候補より先に確保する。
    std::size_t maxStrandedRescueSchedules = 2;
    // Keep most of the global schedule pool for timed multi-meeting routes.
    std::size_t maxStationSchedules = 2;
    std::size_t maxMeetingsPerSchedule = 3;
};

struct RouteColumnResult {
    DailyPlan plan;
    GameState finalState;
    std::size_t generated = 0;
    std::size_t masterNodes = 0;
    std::size_t improvements = 0;
};

namespace detail {

struct RouteColumn {
    AgentPlan plan;
    SpotMask spots = 0;
    BrandMask brands = 0;
    std::int32_t udon = 0;
    CellPos end = kInvalidCell;
    std::int32_t fuel = 0;
};

inline BrandMask columnBrands(const MatchSetting& setting, SpotMask spots) {
    BrandMask result = 0;
    for (std::size_t i = 0; i < setting.field.spots.size(); ++i)
        if ((spots & spotBit(i)) && setting.field.spots[i].brandIndex >= 0)
            result |= BrandMask{1} << setting.field.spots[i].brandIndex;
    return result;
}

inline SpotMask routePathSpots(
    const MatchSetting& setting, const PathResult& path) {
    SpotMask result = 0;
    for (const CellPos pos : path.positions) {
        const int spot = setting.field.at(pos).spotIndex;
        if (spot >= 0)
            result |= spotBit(static_cast<std::size_t>(spot));
    }
    return result;
}

inline std::tuple<int, int, int> columnScore(BrandMask previous,
                                            BrandMask day, int udon) {
    return {countBits(previous | day), countBits(day), udon};
}

} // namespace detail

// A route column is one complete patrol plan. With a fixed supply schedule,
// patrol routes only interact through spot stock, so the master selects one
// column per patrol subject to exact stock-capacity constraints. Every winning
// combination is replayed by the official simulator.
inline RouteColumnResult searchRouteColumns(
    const MatchSetting& setting, const MatchState& matchState, ScoreState previous,
    const DailyPlan& incumbent, const SpotRouteLnsConfig& guard,
    const RouteColumnConfig& config,
    std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    RouteColumnResult result;
    result.plan = incumbent;
    const auto initial = simulateDay(setting, matchState, incumbent, previous);
    if (!initial.valid) return result;
    result.finalState = initial.state;
    if (!config.enabled || !config.labelWidth || !config.columnsPerAgent ||
        !config.maxMasterNodes || Clock::now() >= deadline ||
        matchState.day < 0 || static_cast<std::size_t>(matchState.day) >= setting.daySteps.size())
        return result;
    const int steps = setting.daySteps[static_cast<std::size_t>(matchState.day)];
    const bool finalDay = static_cast<std::size_t>(matchState.day + 1) >= setting.dayCount();
    std::vector<AgentIndex> patrols, supplies;
    for (AgentIndex a = 0; a < matchState.agents.size(); ++a)
        (matchState.agents[a].isPatrol() ? patrols : supplies).push_back(a);
    if (patrols.empty() || setting.field.spots.empty()) return result;

    // Fixed supply schedule plus a small set of deliberate high-demand stations.
    // Rebuild patrol columns for each schedule: their legal fuel trajectories differ.
    std::vector<DailyPlan> supplySchedules{incumbent};
    std::vector<std::array<int, kMaxAgents>> rescueSteps(1);
    const auto appendSupplySchedule = [&](DailyPlan schedule,
                                          std::array<int, kMaxAgents> rescue) {
        if (supplySchedules.size() >= config.maxSupplySchedules) return false;
        const auto duplicate = std::find_if(
            supplySchedules.begin(), supplySchedules.end(),
            [&](const DailyPlan& old) {
                return std::all_of(supplies.begin(), supplies.end(),
                    [&](AgentIndex supply) {
                        return old[supply] == schedule[supply];
                    });
            });
        if (duplicate != supplySchedules.end()) return false;
        supplySchedules.push_back(std::move(schedule));
        rescueSteps.push_back(rescue);
        return true;
    };

    // A fuel-zero patrol cannot appear in a useful incumbent meeting trace:
    // it simply waits while generic schedules are ranked around moving cars.
    // Reserve direct-to-current-position rescues before consuming the global
    // schedule pool with station and timed-meeting alternatives.
    if (config.supplyRendezvous && !supplies.empty() &&
        config.maxStrandedRescueSchedules > 0 &&
        supplySchedules.size() < config.maxSupplySchedules) {
        struct Rescue {
            std::int64_t value = 0;
            AgentIndex supply = 0;
            AgentIndex patrol = 0;
            PathResult path;
        };
        std::vector<Rescue> rescues;
        std::array<DijkstraTable, kMaxAgents> supplyTables;
        for (const AgentIndex supply : supplies)
            supplyTables[supply] = Brain0::dijkstra(
                setting, matchState, matchState.agents[supply].pos);
        const int lowFuel = std::max(2, setting.fuelLimit / 10);
        for (const AgentIndex patrol : patrols) {
            const auto& agent = matchState.agents[patrol];
            bool canMove = false;
            for (int raw = 0; raw < 6; ++raw) {
                const auto direction = static_cast<Direction>(raw);
                const CellPos next = setting.field.neighbor(agent.pos, direction);
                if (!setting.field.contains(next) ||
                    !setting.field.at(next).isPassable()) continue;
                if (moveFuelCost(setting, AgentKind::Patrol, agent.pos) <=
                    agent.fuel) {
                    canMove = true;
                    break;
                }
            }
            if (canMove && agent.fuel > lowFuel) continue;
            for (const AgentIndex supply : supplies) {
                const auto path = Brain0::restorePath(
                    setting, supplyTables[supply], agent.pos,
                    AgentKind::Supply);
                if (!path.reachable || path.cost.steps >= steps) continue;
                const std::int64_t urgency = canMove ? setting.fuelLimit :
                    static_cast<std::int64_t>(setting.fuelLimit) * steps;
                const std::int64_t value = urgency +
                    static_cast<std::int64_t>(setting.fuelLimit - agent.fuel) *
                        (steps - path.cost.steps) - path.cost.steps;
                rescues.push_back({value, supply, patrol, path});
            }
        }
        std::stable_sort(rescues.begin(), rescues.end(),
            [](const Rescue& lhs, const Rescue& rhs) {
                return std::make_tuple(lhs.value, -lhs.path.cost.steps,
                           -static_cast<int>(lhs.patrol),
                           -static_cast<int>(lhs.supply)) >
                       std::make_tuple(rhs.value, -rhs.path.cost.steps,
                           -static_cast<int>(rhs.patrol),
                           -static_cast<int>(rhs.supply));
            });
        const auto rescuePlan = [&](const Rescue& rescue) {
            AgentPlan actions;
            for (const Direction direction : rescue.path.directions)
                actions.push_back(static_cast<int>(direction));
            if (rescue.path.cost.steps < steps)
                actions.push_back(rescue.path.cost.steps - steps);
            return actions;
        };
        // With several supply vehicles, first offer one schedule that rescues
        // different patrols in parallel instead of mutually exclusive plans.
        if (!rescues.empty()) {
            DailyPlan combined = incumbent;
            std::array<int, kMaxAgents> combinedSteps{};
            std::array<bool, kMaxAgents> usedSupply{}, usedPatrol{};
            for (const Rescue& rescue : rescues) {
                if (usedSupply[rescue.supply] || usedPatrol[rescue.patrol])
                    continue;
                combined[rescue.supply] = rescuePlan(rescue);
                // Refuelling is applied after step 1 even when both vehicles
                // start on the same cell.  The patrol must wait through that
                // step before a fuel-consuming route becomes legal.
                combinedSteps[rescue.patrol] =
                    std::max(1, rescue.path.cost.steps);
                usedSupply[rescue.supply] = true;
                usedPatrol[rescue.patrol] = true;
            }
            appendSupplySchedule(std::move(combined), combinedSteps);
        }
        std::size_t added = 0;
        for (const Rescue& rescue : rescues) {
            if (added >= config.maxStrandedRescueSchedules ||
                supplySchedules.size() >= config.maxSupplySchedules) break;
            DailyPlan schedule = incumbent;
            schedule[rescue.supply] = rescuePlan(rescue);
            std::array<int, kMaxAgents> stepsToRescue{};
            stepsToRescue[rescue.patrol] =
                std::max(1, rescue.path.cost.steps);
            if (appendSupplySchedule(std::move(schedule), stepsToRescue))
                ++added;
        }
    }
    if (config.supplyRendezvous && !supplies.empty() &&
        config.maxStationSchedules > 0) {
        std::vector<DijkstraTable> patrolStartTables;
        patrolStartTables.reserve(patrols.size());
        for (const AgentIndex patrol : patrols)
            patrolStartTables.push_back(Brain0::dijkstra(
                setting, matchState, matchState.agents[patrol].pos));
        struct Station {
            std::int64_t value = 0;
            AgentIndex supply = 0;
            std::size_t spot = 0;
            PathResult path;
        };
        std::vector<Station> stations;
        for (const AgentIndex supply : supplies) {
            const auto table = Brain0::dijkstra(
                setting, matchState, matchState.agents[supply].pos);
            for (std::size_t i = 0; i < setting.field.spots.size(); ++i) {
                const auto& spot = setting.field.spots[i];
                const auto path = Brain0::restorePath(
                    setting, table, spot.pos, AgentKind::Supply);
                if (!path.reachable || path.cost.steps > steps / 2) continue;
                std::int64_t delivered = 0;
                int served = 0;
                for (std::size_t p = 0; p < patrols.size(); ++p) {
                    const auto patrolPath = Brain0::restorePath(
                        setting, patrolStartTables[p], spot.pos);
                    if (!patrolPath.reachable ||
                        patrolPath.cost.steps > steps / 2 ||
                        patrolPath.cost.fuel >
                            matchState.agents[patrols[p]].fuel) continue;
                    const int fuelOnArrival =
                        matchState.agents[patrols[p]].fuel -
                        patrolPath.cost.fuel;
                    const int missing = std::max(
                        0, setting.fuelLimit - fuelOnArrival);
                    if (missing == 0) continue;
                    const int serviceTime = std::max(
                        path.cost.steps, patrolPath.cost.steps);
                    delivered += static_cast<std::int64_t>(missing) *
                        std::max(1, steps - serviceTime + 1);
                    ++served;
                }
                // A parked supply with no predicted customer is merely a
                // positional guess.  Leave such repositioning to the
                // overnight refinement instead of consuming schedule slots.
                if (served == 0) continue;
                const auto stockUse = std::min(
                    spot.maxStock, static_cast<int>(patrols.size()));
                const std::int64_t value = delivered +
                    static_cast<std::int64_t>(served) * setting.fuelLimit +
                    static_cast<std::int64_t>(stockUse) * steps -
                    path.cost.steps;
                stations.push_back({value, supply, i, path});
            }
        }
        std::stable_sort(stations.begin(), stations.end(),
            [](const Station& lhs, const Station& rhs) {
                return std::make_tuple(lhs.value, -lhs.path.cost.steps,
                           -static_cast<int>(lhs.supply),
                           -static_cast<int>(lhs.spot)) >
                       std::make_tuple(rhs.value, -rhs.path.cost.steps,
                           -static_cast<int>(rhs.supply),
                           -static_cast<int>(rhs.spot));
            });
        const auto stationLimit = std::min({
            config.maxStationSchedules, stations.size(),
            config.maxSupplySchedules > supplySchedules.size()
                ? config.maxSupplySchedules - supplySchedules.size()
                : std::size_t{0}});
        for (std::size_t candidate = 0;
             candidate < stationLimit; ++candidate) {
            const auto& station = stations[candidate];
            DailyPlan schedule = incumbent;
            AgentPlan actions;
            for (const Direction d : station.path.directions)
                actions.push_back(static_cast<int>(d));
            if (steps > station.path.cost.steps)
                actions.push_back(station.path.cost.steps - steps);
            schedule[station.supply] = std::move(actions);
            if (schedule[station.supply] != incumbent[station.supply] &&
                std::none_of(supplySchedules.begin(), supplySchedules.end(),
                    [&](const auto& old) {
                        return old[station.supply] == schedule[station.supply];
                    })) {
                appendSupplySchedule(std::move(schedule), {});
            }
        }
    }

    // Joint neighbourhood: move a supply to timed patrol meetings, then
    // regenerate every patrol's columns under that new fuel timeline.
    if (config.supplyRendezvous && !supplies.empty() &&
        supplySchedules.size() < config.maxSupplySchedules) {
        const auto trace = detail::repairTrace(setting, matchState, previous, incumbent,
            [&] { return Clock::now() >= deadline; });
        struct Meeting { CellPos pos; int time; int demand; };
        std::vector<Meeting> meetings;
        for (std::size_t t = 1; t < trace.checkpoints.size(); t += 2) {
            const auto& state = trace.checkpoints[t].state;
            for (const auto p : patrols) {
                const auto pos = state.agents[p].pos;
                int demand = 0;
                for (const auto q : patrols)
                    if (state.agents[q].pos == pos)
                        demand += setting.fuelLimit - state.agents[q].fuel;
                if (demand <= 0) continue;
                const auto duplicate = std::find_if(
                    meetings.begin(), meetings.end(), [&](const Meeting& old) {
                        return old.pos == pos && old.time == static_cast<int>(t);
                    });
                if (duplicate == meetings.end()) {
                    meetings.push_back({pos, static_cast<int>(t), demand});
                } else {
                    duplicate->demand = std::max(duplicate->demand, demand);
                }
            }
        }
        // Prefer fuel delivered while there is still useful working time.
        // Sorting only by missing fuel repeatedly targeted late, stranded cars.
        std::stable_sort(meetings.begin(), meetings.end(), [steps](const auto& a, const auto& b) {
            return std::make_tuple(1LL * a.demand * (steps - a.time + 1), -a.time) >
                   std::make_tuple(1LL * b.demand * (steps - b.time + 1), -b.time);
        });
        for (const auto supply : supplies) {
            if (Clock::now() >= deadline) break;
            const auto table = Brain0::dijkstra(setting, matchState, matchState.agents[supply].pos);
            for (const auto& first : meetings) for (int keepPrefix = 0; keepPrefix < 2; ++keepPrefix) {
                if (supplySchedules.size() >= config.maxSupplySchedules || Clock::now() >= deadline) break;
                AgentPlan plan;
                const int cut = keepPrefix ? first.time / 2 : 0;
                const auto& cp = trace.checkpoints[static_cast<std::size_t>(cut)];
                const auto& pending = cp.state.pendingActions[supply];
                if (pending.isMove()) continue;
                if (cut > 0) {
                    plan.assign(incumbent[supply].begin(), incumbent[supply].begin() + cp.cursors[supply]);
                    if (pending.isWait()) {
                        const int elapsed = -plan.back() - pending.remainingSteps;
                        if (elapsed > 0) plan.back() = -elapsed;
                        else plan.pop_back();
                    }
                }
                const auto prefixTable = cut > 0 ? Brain0::dijkstra(setting, matchState,
                    cp.state.agents[supply].pos) : table;
                const auto path = Brain0::restorePath(setting, prefixTable, first.pos, AgentKind::Supply);
                if (!path.reachable || cut + path.cost.steps > first.time || first.time >= steps) continue;
                for (const auto d : path.directions) plan.push_back(static_cast<int>(d));
                int time = std::min(steps, first.time + 1);
                if (time > cut + path.cost.steps) plan.push_back(cut + path.cost.steps - time);
                CellPos current = first.pos;
                for (std::size_t leg = 1;
                     leg < config.maxMeetingsPerSchedule; ++leg) {
                    const auto onward = Brain0::dijkstra(
                        setting, matchState, current);
                    const Meeting* selected = nullptr;
                    PathResult selectedPath;
                    std::int64_t selectedValue =
                        std::numeric_limits<std::int64_t>::min();
                    for (const auto& nextMeeting : meetings) {
                        if (nextMeeting.time < time) continue;
                        const auto next = Brain0::restorePath(
                            setting, onward, nextMeeting.pos,
                            AgentKind::Supply);
                        if (!next.reachable ||
                            time + next.cost.steps > nextMeeting.time) continue;
                        const std::int64_t value =
                            static_cast<std::int64_t>(nextMeeting.demand) *
                                (steps - nextMeeting.time + 1) -
                            next.cost.steps;
                        if (!selected || value > selectedValue ||
                            (value == selectedValue &&
                             std::make_pair(nextMeeting.time, nextMeeting.pos) <
                                 std::make_pair(selected->time, selected->pos))) {
                            selected = &nextMeeting;
                            selectedPath = next;
                            selectedValue = value;
                        }
                    }
                    if (!selected) break;
                    for (const auto d : selectedPath.directions)
                        plan.push_back(static_cast<int>(d));
                    time += selectedPath.cost.steps;
                    const int leave = std::min(steps, selected->time + 1);
                    if (leave > time) plan.push_back(time - leave);
                    time = leave;
                    current = selected->pos;
                }
                if (time < steps) plan.push_back(time - steps);
                if (std::any_of(supplySchedules.begin(), supplySchedules.end(),
                    [&](const auto& old) { return old[supply] == plan; })) continue;
                auto schedule = incumbent;
                schedule[supply] = std::move(plan);
                appendSupplySchedule(std::move(schedule), {});
            }
        }
    }

    const auto scoreOf = [&](const GameState& state) {
        return detail::columnScore(previous.matchBrands, state.dayBrands,
                                   state.score.totalUdon);
    };
    std::vector<DijkstraTable> futureCache(setting.field.cellCount());
    std::vector<bool> futureCached(setting.field.cellCount(), false);
    const auto initialFuture = !finalDay && guard.allowFuturePositionChange
        ? detail::futureRoutePotential(setting, matchState, initial.state,
                                       futureCache, futureCached)
        : detail::FutureRoutePotential{};
    const auto preservesFuture = [&](const GameState& candidate) {
        if (finalDay || !guard.preserveIntermediateState) return true;
        if ((candidate.score.matchBrands & initial.state.score.matchBrands) !=
                initial.state.score.matchBrands ||
            (candidate.dayBrands & initial.state.dayBrands) !=
                initial.state.dayBrands) return false;
        if (!guard.allowFuturePositionChange) {
            for (AgentIndex a = 0; a < candidate.agentCount; ++a)
                if (candidate.agents[a].pos != initial.state.agents[a].pos ||
                    candidate.agents[a].fuel < initial.state.agents[a].fuel)
                    return false;
            return true;
        }
        const auto future = detail::futureRoutePotential(setting, matchState,
            candidate, futureCache, futureCached);
        const int extra = candidate.score.totalUdon - initial.state.score.totalUdon;
        return future.reachableBrands >= initialFuture.reachableBrands &&
            future.strandedPatrols <= initialFuture.strandedPatrols &&
            future.distanceSum <= initialFuture.distanceSum + 16 + 8 * extra &&
            future.patrolFuel >= initialFuture.patrolFuel - 20 - 30 * extra;
    };
    const auto futureRank = [&](const GameState& candidate) {
        if (finalDay || !guard.allowFuturePositionChange)
            return std::make_tuple(0, 0, 0, 0, 0, 0);
        const auto future = detail::futureRoutePotential(setting, matchState,
            candidate, futureCache, futureCached);
        return std::make_tuple(future.reachableBrands,
            -future.strandedPatrols, future.readyBrands, future.readyUdon,
            -future.distanceSum, future.patrolFuel);
    };

    // Paths between the patrol starts and all spots are shared by all supply
    // schedules. Labels do not assume fuel feasibility: solo simulation checks
    // refuels at their actual step and rejects impossible columns.
    std::vector<DijkstraTable> paths(setting.field.cellCount());
    std::vector<bool> pathCached(setting.field.cellCount(), false);
    const auto pathTo = [&](CellPos from, CellPos to) {
        const auto index = static_cast<std::size_t>(from);
        if (!pathCached[index]) {
            paths[index] = Brain0::dijkstra(setting, matchState, from);
            pathCached[index] = true;
        }
        return Brain0::restorePath(setting, paths[index], to);
    };
    CellPos neutral = kInvalidCell;
    for (CellPos p = 0; p < static_cast<CellPos>(setting.field.cellCount()); ++p)
        if (setting.field.at(p).isPassable() && !setting.field.at(p).hasSpot()) {
            neutral = p;
            break;
        }
    const auto incumbentTrace = detail::repairTrace(setting, matchState,
        previous, incumbent, [] { return false; });

    for (std::size_t scheduleIndex = 0;
         scheduleIndex < supplySchedules.size(); ++scheduleIndex) {
        if (Clock::now() >= deadline) break;
        const DailyPlan& schedule = supplySchedules[scheduleIndex];
        const bool rescueSchedule = std::any_of(
            rescueSteps[scheduleIndex].begin(),
            rescueSteps[scheduleIndex].end(),
            [](int rescueStep) { return rescueStep > 0; });
        const auto scheduleDeadline = Clock::now() +
            (deadline - Clock::now()) /
                static_cast<std::int64_t>(supplySchedules.size() - scheduleIndex);
        const auto generationDeadline = Clock::now() +
            (scheduleDeadline - Clock::now()) / 3 * 2;
        std::vector<std::vector<detail::RouteColumn>> pools(patrols.size());
        bool complete = true;
        for (std::size_t pi = 0; pi < patrols.size() && Clock::now() < scheduleDeadline; ++pi) {
            const auto agentDeadline = Clock::now() +
                std::max(Clock::duration::zero(), generationDeadline - Clock::now()) /
                    static_cast<std::int64_t>(patrols.size() - pi);
            const AgentIndex agent = patrols[pi];
            MatchState soloState = matchState;
            if (neutral != kInvalidCell)
                for (const AgentIndex other : patrols)
                    if (other != agent) soloState.agents[other].pos = neutral;
            DailyPlan trial(matchState.agents.size(), AgentPlan{-steps});
            for (const AgentIndex supply : supplies) trial[supply] = schedule[supply];
            const auto addColumn = [&](const AgentPlan& actions) {
                trial[agent] = actions;
                const auto sim = simulateDay(setting, soloState, trial, previous);
                ++result.generated;
                if (!sim.valid) return false;
                detail::RouteColumn column;
                column.plan = actions;
                column.spots = sim.state.acquiredSpots[agent];
                column.brands = detail::columnBrands(setting, column.spots);
                column.udon = countBits(column.spots);
                column.end = sim.state.agents[agent].pos;
                column.fuel = sim.state.agents[agent].fuel;
                auto& pool = pools[pi];
                for (auto& old : pool)
                    if (old.spots == column.spots && old.end == column.end &&
                        old.fuel == column.fuel) return true;
                pool.push_back(std::move(column));
                return true;
            };
            addColumn(incumbent[agent]);
            addColumn(AgentPlan{-steps});
            // Preserve the incumbent's timed refuels and spend any terminal
            // wait on one more spot. Rebuilding the entire route would lose
            // those carefully scheduled rendezvous.
            if (!incumbent[agent].empty() && incumbent[agent].back() < 0) {
                const int spare = -incumbent[agent].back();
                const CellPos end = initial.state.agents[agent].pos;
                for (std::size_t spot = 0;
                     spot < setting.field.spots.size() && Clock::now() < agentDeadline;
                     ++spot) {
                    if (initial.state.acquiredSpots[agent] & spotBit(spot)) continue;
                    const auto path = pathTo(end, setting.field.spots[spot].pos);
                    const int duration = path.cost.steps + (path.cost.steps == 0);
                    if (!path.reachable || duration > spare) continue;
                    AgentPlan actions = incumbent[agent];
                    actions.pop_back();
                    for (const Direction d : path.directions)
                        actions.push_back(static_cast<int>(d));
                    if (path.cost.steps == 0) actions.push_back(-1);
                    if (duration < spare) actions.push_back(duration - spare);
                    addColumn(actions);
                }
            }
            // Preserve the original timed-refuel prefix at several action
            // boundaries, and rebuild only its suffix. Whole-route rebuilding
            // often invalidates otherwise useful high-pickup patrol routes.
            if (!incumbentTrace.checkpoints.empty()) {
                std::vector<std::size_t> usedCuts;
                const std::array<int, 4> cutFractions{{3, 2, 1, 0}};
                for (const int fraction : cutFractions) {
                    if (Clock::now() >= agentDeadline) break;
                    std::size_t cut = std::min<std::size_t>(
                        incumbentTrace.checkpoints.size() - 1,
                        static_cast<std::size_t>(steps * fraction / 4));
                    while (cut > 0 && incumbentTrace.checkpoints[cut]
                            .state.pendingActions[agent].active) --cut;
                    if (std::find(usedCuts.begin(), usedCuts.end(), cut) !=
                        usedCuts.end()) continue;
                    usedCuts.push_back(cut);
                    const auto& checkpoint = incumbentTrace.checkpoints[cut];
                    const auto base = detail::acquiredSpotSequence(
                        incumbentTrace, initial.state, agent, cut);
                    const CellPos start = checkpoint.state.agents[agent].pos;
                    const int left = checkpoint.state.remainingSteps();
                    const auto prefixEnd = incumbent[agent].begin() +
                        static_cast<std::ptrdiff_t>(checkpoint.cursors[agent]);
                    const AgentPlan prefix(incumbent[agent].begin(), prefixEnd);
                    const auto buildVisits = [&](const std::vector<std::size_t>& visits) {
                        CellPos from = start;
                        int used = 0;
                        AgentPlan actions = prefix;
                        for (const std::size_t spot : visits) {
                            const auto path = pathTo(from, setting.field.spots[spot].pos);
                            const int duration = path.cost.steps + (path.cost.steps == 0);
                            if (!path.reachable || used + duration > left) return;
                            for (const Direction d : path.directions)
                                actions.push_back(static_cast<int>(d));
                            if (path.cost.steps == 0) actions.push_back(-1);
                            used += duration;
                            from = setting.field.spots[spot].pos;
                        }
                        if (used < left) actions.push_back(used - left);
                        addColumn(actions);
                    };
                    for (std::size_t spot = 0;
                         spot < setting.field.spots.size() && Clock::now() < agentDeadline;
                         ++spot) {
                        if ((checkpoint.state.acquiredSpots[agent] & spotBit(spot)) ||
                            std::find(base.begin(), base.end(), spot) != base.end()) continue;
                        std::vector<std::pair<int, std::size_t>> positions;
                        for (std::size_t insert = 0; insert <= base.size(); ++insert) {
                            const CellPos before = insert == 0
                                ? start
                                : setting.field.spots[base[insert - 1]].pos;
                            const CellPos target = setting.field.spots[spot].pos;
                            const int detour = pathTo(before, target).cost.steps +
                                (insert == base.size() ? 0 :
                                 pathTo(target, setting.field.spots[base[insert]].pos).cost.steps -
                                 pathTo(before, setting.field.spots[base[insert]].pos).cost.steps);
                            positions.push_back({detour, insert});
                        }
                        std::sort(positions.begin(), positions.end());
                        for (std::size_t k = 0;
                             k < std::min<std::size_t>(2, positions.size()); ++k) {
                            if (Clock::now() >= agentDeadline) break;
                            auto visits = base;
                            visits.insert(visits.begin() + positions[k].second, spot);
                            buildVisits(visits);
                        }
                        if (!base.empty() && Clock::now() < agentDeadline) {
                            // Replacing an expensive suffix stop can free time
                            // for a brand that the team still lacks.
                            int bestCost = kImpassableCost;
                            std::size_t bestReplace = base.size();
                            for (std::size_t remove = 0; remove < base.size(); ++remove) {
                                const CellPos before = remove == 0 ? start :
                                    setting.field.spots[base[remove - 1]].pos;
                                const CellPos after = remove + 1 == base.size()
                                    ? kInvalidCell : setting.field.spots[base[remove + 1]].pos;
                                const CellPos target = setting.field.spots[spot].pos;
                                const int cost = pathTo(before, target).cost.steps +
                                    (after == kInvalidCell ? 0 :
                                     pathTo(target, after).cost.steps);
                                if (cost < bestCost) {
                                    bestCost = cost;
                                    bestReplace = remove;
                                }
                            }
                            if (bestReplace < base.size()) {
                                auto visits = base;
                                visits[bestReplace] = spot;
                                buildVisits(visits);
                            }
                        }
                    }
                }
            }
            struct Label {
                CellPos pos = kInvalidCell;
                AgentPlan actions;
                SpotMask visited = 0;
                BrandMask brands = 0;
                int time = 0;
                int count = 0;
            };
            std::vector<Label> beam{{matchState.agents[agent].pos, {}, 0, 0, 0, 0}};
            const int rescueStep = rescueSteps[scheduleIndex][agent];
            if (rescueStep > 0 && rescueStep < steps) {
                beam.push_back({matchState.agents[agent].pos,
                                AgentPlan{-rescueStep}, 0, 0,
                                rescueStep, 0});
            }
            for (std::size_t depth = 0;
                 depth < config.maxVisits && !beam.empty() &&
                     Clock::now() < agentDeadline;
                 ++depth) {
                std::vector<Label> next;
                for (const Label& label : beam) {
                    for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot) {
                        if (label.visited & spotBit(spot)) continue;
                        const auto& target = setting.field.spots[spot];
                        const auto path = pathTo(label.pos, target.pos);
                        const int duration = path.cost.steps + (path.cost.steps == 0);
                        if (!path.reachable || label.time + duration > steps) continue;
                        Label child = label;
                        child.pos = target.pos;
                        child.time += duration;
                        if (config.rankTransitPickups &&
                            setting.field.width >= 32 &&
                            setting.field.height >= 32) {
                            const SpotMask traversed =
                                detail::routePathSpots(setting, path);
                            const SpotMask gained = traversed & ~child.visited;
                            child.visited |= traversed;
                            child.count += countBits(gained);
                            child.brands |= detail::columnBrands(
                                setting, traversed);
                        } else {
                            child.visited |= spotBit(spot);
                            if (target.brandIndex >= 0)
                                child.brands |= BrandMask{1} <<
                                    target.brandIndex;
                            ++child.count;
                        }
                        for (const Direction d : path.directions)
                            child.actions.push_back(static_cast<int>(d));
                        if (path.cost.steps == 0) child.actions.push_back(-1);
                        next.push_back(std::move(child));
                    }
                }
                std::sort(next.begin(), next.end(), [&](const Label& a, const Label& b) {
                    const auto rank = [&](const Label& x) {
                        return std::make_tuple(
                            countBits(x.brands & ~previous.matchBrands),
                            countBits(x.brands), x.count, -x.time);
                    };
                    return rank(a) > rank(b);
                });
                if (next.size() > config.labelWidth) {
                    const std::size_t brandQuota = std::max<std::size_t>(
                        1, config.labelWidth / 2);
                    std::sort(next.begin() + brandQuota, next.end(),
                        [&](const Label& a, const Label& b) {
                            const auto rank = [&](const Label& x) {
                                return std::make_tuple(
                                    -x.time,
                                    countBits(x.brands & ~previous.matchBrands),
                                    countBits(x.brands),
                                    x.count);
                            };
                            return rank(a) > rank(b);
                        });
                }
                if (next.size() > config.labelWidth) next.resize(config.labelWidth);
                beam.clear();
                for (Label& label : next) {
                    if (Clock::now() >= agentDeadline) break;
                    AgentPlan completePlan = label.actions;
                    if (label.time < steps) completePlan.push_back(label.time - steps);
                    const bool valid = addColumn(completePlan);
                    // An invalid prefix cannot become fuel-feasible by appending
                    // later actions. Keep valid labels for the next visit depth.
                    if (valid) beam.push_back(std::move(label));
                }
            }
            auto& pool = pools[pi];
            const auto incumbentIt = std::find_if(pool.begin(), pool.end(),
                [&](const detail::RouteColumn& col) {
                    return col.plan == incumbent[agent];
                });
            const bool hasIncumbent = incumbentIt != pool.end();
            detail::RouteColumn incumbentColumn;
            if (hasIncumbent) incumbentColumn = *incumbentIt;
            if (pool.empty()) { complete = false; break; }
            const auto brandRank = [&](const detail::RouteColumn& column) {
                return std::make_tuple(
                    countBits(column.brands & ~previous.matchBrands),
                    countBits(column.brands), column.udon, column.fuel);
            };
            const auto quantityRank = [&](const detail::RouteColumn& column) {
                return std::make_tuple(
                    column.udon,
                    countBits(column.brands & ~previous.matchBrands),
                    countBits(column.brands), column.fuel);
            };
            // Plan each patrol for pickup quantity first. Keep the best brand
            // route outside that quota so the master can repair a missing
            // brand after seeing the routes of all patrols together.
            const auto bestBrand = *std::max_element(pool.begin(), pool.end(),
                [&](const auto& a, const auto& b) {
                    return brandRank(a) < brandRank(b);
                });
            std::sort(pool.begin(), pool.end(), [&](const auto& a, const auto& b) {
                return quantityRank(a) > quantityRank(b);
            });
            if (pool.size() > config.columnsPerAgent) {
                std::vector<detail::RouteColumn> selected;
                selected.reserve(config.columnsPerAgent);
                std::vector<SpotMask> masks;
                std::array<int, kMaxSpots> otherUses{};
                BrandMask otherBrands = 0;
                for (const AgentIndex other : patrols) {
                    if (other == agent) continue;
                    const SpotMask acquired = initial.state.acquiredSpots[other];
                    otherBrands |= detail::columnBrands(setting, acquired);
                    for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot)
                        if (acquired & spotBit(spot)) ++otherUses[spot];
                }
                const auto take = [&](const detail::RouteColumn& column) {
                    if (std::count(masks.begin(), masks.end(), column.spots) >= 2)
                        return;
                    if (std::any_of(selected.begin(), selected.end(),
                        [&](const detail::RouteColumn& existing) {
                            return existing.spots == column.spots &&
                                existing.end == column.end &&
                                existing.fuel == column.fuel;
                        })) return;
                    masks.push_back(column.spots);
                    selected.push_back(column);
                };
                const std::size_t quantityQuota = std::max<std::size_t>(
                    1, config.columnsPerAgent * 2 / 3);
                const std::size_t brandQuota = std::min(config.columnsPerAgent,
                    quantityQuota + std::max<std::size_t>(
                        1, config.columnsPerAgent / 6));
                for (auto& column : pool) {
                    take(column);
                    if (selected.size() >= quantityQuota) break;
                }
                std::sort(pool.begin(), pool.end(), [&](const auto& a, const auto& b) {
                    return brandRank(a) > brandRank(b);
                });
                for (auto& column : pool) {
                    if (selected.size() >= brandQuota) break;
                    take(column);
                }
                const auto marginal = [&](const detail::RouteColumn& column) {
                    int uncovered = 0;
                    for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot)
                        if ((column.spots & spotBit(spot)) &&
                            otherUses[spot] < setting.field.spots[spot].maxStock)
                            ++uncovered;
                    return std::make_tuple(
                        countBits(column.brands &
                                  ~(previous.matchBrands | otherBrands)),
                        countBits(column.brands & ~otherBrands), uncovered,
                        column.udon, column.fuel);
                };
                std::sort(pool.begin(), pool.end(), [&](const auto& a, const auto& b) {
                    return marginal(a) > marginal(b);
                });
                for (auto& column : pool) {
                    if (selected.size() == config.columnsPerAgent) break;
                    take(column);
                }
                pool = std::move(selected);
            }
            if (std::none_of(pool.begin(), pool.end(),
                    [&](const detail::RouteColumn& col) {
                        return col.plan == bestBrand.plan;
                    })) {
                pool.push_back(bestBrand);
            }
            if (hasIncumbent && !pool.empty() &&
                std::none_of(pool.begin(), pool.end(),
                    [&](const detail::RouteColumn& col) {
                        return col.plan == incumbent[agent];
                    })) {
                // The incumbent is a safety column, not part of the search
                // quota. Replacing the last selected route would remove the
                // only improvement when columnsPerAgent is small.
                pool.push_back(std::move(incumbentColumn));
            }
            if (pool.empty()) { complete = false; break; }
        }
        if (!complete || Clock::now() >= scheduleDeadline) continue;
        std::vector<BrandMask> remaining(patrols.size() + 1, 0);
        std::vector<int> maxUdon(patrols.size() + 1, 0);
        for (std::size_t i = patrols.size(); i-- > 0;) {
            remaining[i] = remaining[i + 1];
            int best = 0;
            for (const auto& col : pools[i]) {
                remaining[i] |= col.brands;
                best = std::max(best, col.udon);
            }
            maxUdon[i] = maxUdon[i + 1] + best;
        }
        std::array<int, kMaxSpots> used{};
        DailyPlan combined = schedule;
        const std::size_t masterStart = result.masterNodes;
        const std::size_t scheduleLimit = std::max<std::size_t>(1,
            config.maxMasterNodes / supplySchedules.size());
        std::function<void(std::size_t, BrandMask, int)> select;
        select = [&](std::size_t index, BrandMask brands, int udon) {
            if (Clock::now() >= scheduleDeadline ||
                result.masterNodes - masterStart >= scheduleLimit)
                return;
            ++result.masterNodes;
            BrandMask availableBrands = 0;
            int remainingStock = 0;
            for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot) {
                const int available = setting.field.spots[spot].maxStock - used[spot];
                if (available <= 0) continue;
                remainingStock += available;
                const int brand = setting.field.spots[spot].brandIndex;
                if (brand >= 0) availableBrands |= brandBit(brand);
            }
            const auto optimistic = detail::columnScore(previous.matchBrands,
                brands | (remaining[index] & availableBrands),
                previous.totalUdon + udon +
                    std::min(maxUdon[index], remainingStock));
            // Equality is not a dead branch on intermediate days: a direct
            // rescue can preserve today's official score while preventing a
            // stranded patrol tomorrow.
            if (optimistic < scoreOf(result.finalState) ||
                (optimistic == scoreOf(result.finalState) &&
                 (!rescueSchedule || finalDay))) return;
            if (index == patrols.size()) {
                // A high-pickup column is provisional until every patrol has
                // a route. The full replay decides whether brand-repair
                // columns are needed; never accept a brand regression.
                const auto checked = simulateDay(setting, matchState, combined, previous);
                const bool betterOfficial = checked.valid &&
                    scoreOf(checked.state) > scoreOf(result.finalState);
                const bool betterFuture = checked.valid && rescueSchedule &&
                    !finalDay &&
                    scoreOf(checked.state) == scoreOf(result.finalState) &&
                    futureRank(checked.state) > futureRank(result.finalState);
                if ((betterOfficial || betterFuture) &&
                    preservesFuture(checked.state)) {
                    result.plan = combined;
                    result.finalState = checked.state;
                    ++result.improvements;
                }
                return;
            }
            const AgentIndex agent = patrols[index];
            std::vector<const detail::RouteColumn*> choices;
            choices.reserve(pools[index].size());
            for (const auto& col : pools[index]) {
                bool fits = true;
                for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot)
                    if ((col.spots & spotBit(spot)) &&
                        used[spot] >= setting.field.spots[spot].maxStock) {
                        fits = false;
                        break;
                    }
                if (fits) choices.push_back(&col);
            }
            std::sort(choices.begin(), choices.end(), [&](const auto* lhs,
                                                           const auto* rhs) {
                return std::make_tuple(
                    lhs->udon,
                    countBits(lhs->brands &
                              ~(previous.matchBrands | brands)),
                    countBits(lhs->brands & ~brands), lhs->fuel) >
                       std::make_tuple(
                    rhs->udon,
                    countBits(rhs->brands &
                              ~(previous.matchBrands | brands)),
                    countBits(rhs->brands & ~brands), rhs->fuel);
            });
            for (const auto* choice : choices) {
                const auto& col = *choice;
                for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot)
                    if (col.spots & spotBit(spot)) ++used[spot];
                combined[agent] = col.plan;
                select(index + 1, brands | col.brands, udon + col.udon);
                for (std::size_t spot = 0; spot < setting.field.spots.size(); ++spot)
                    if (col.spots & spotBit(spot)) --used[spot];
                if (Clock::now() >= scheduleDeadline ||
                    result.masterNodes - masterStart >= scheduleLimit)
                    break;
            }
        };
        select(0, 0, 0);
    }
    return result;
}

} // namespace benzen
