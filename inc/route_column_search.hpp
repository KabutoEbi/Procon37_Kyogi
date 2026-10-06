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
    bool supplyRendezvous = true;
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

    // Fixed supply schedule plus one deliberate station at a high-demand spot.
    // Rebuild patrol columns for each schedule: their legal fuel trajectories differ.
    std::vector<DailyPlan> supplySchedules{incumbent};
    if (config.supplyRendezvous && !supplies.empty()) {
        const AgentIndex supply = supplies.front();
        const auto table = Brain0::dijkstra(setting, matchState,
                                             matchState.agents[supply].pos);
        std::vector<DijkstraTable> patrolStartTables;
        patrolStartTables.reserve(patrols.size());
        for (const AgentIndex patrol : patrols)
            patrolStartTables.push_back(Brain0::dijkstra(
                setting, matchState, matchState.agents[patrol].pos));
        int bestValue = -1;
        std::size_t bestSpot = setting.field.spots.size();
        for (std::size_t i = 0; i < setting.field.spots.size(); ++i) {
            const auto& spot = setting.field.spots[i];
            const auto path = Brain0::restorePath(setting, table, spot.pos,
                                                   AgentKind::Supply);
            if (!path.reachable || path.cost.steps > steps / 2) continue;
            int value = std::min(spot.maxStock, static_cast<int>(patrols.size())) * 2 -
                        path.cost.steps;
            for (std::size_t p = 0; p < patrols.size(); ++p) {
                const auto patrolPath = Brain0::restorePath(setting,
                    patrolStartTables[p], spot.pos);
                if (!patrolPath.reachable || patrolPath.cost.steps > steps / 2 ||
                    patrolPath.cost.fuel > matchState.agents[patrols[p]].fuel)
                    continue;
                const int fuelOnArrival = matchState.agents[patrols[p]].fuel -
                    patrolPath.cost.fuel;
                const int missing = std::max(0, setting.fuelLimit - fuelOnArrival);
                value += 3 + missing * 4 / std::max(1, setting.fuelLimit);
            }
            if (bestSpot == setting.field.spots.size() || value > bestValue) {
                bestValue = value;
                bestSpot = i;
            }
        }
        if (bestSpot < setting.field.spots.size()) {
            const auto path = Brain0::restorePath(setting, table,
                setting.field.spots[bestSpot].pos, AgentKind::Supply);
            DailyPlan schedule = incumbent;
            AgentPlan actions;
            for (const Direction d : path.directions)
                actions.push_back(static_cast<int>(d));
            if (steps > path.cost.steps) actions.push_back(path.cost.steps - steps);
            schedule[supply] = std::move(actions);
            if (schedule[supply] != incumbent[supply]) supplySchedules.push_back(schedule);
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
            future.distanceSum <= initialFuture.distanceSum + 16 + 8 * extra &&
            future.patrolFuel >= initialFuture.patrolFuel - 20 - 30 * extra;
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
                        child.visited |= spotBit(spot);
                        if (target.brandIndex >= 0)
                            child.brands |= BrandMask{1} << target.brandIndex;
                        ++child.count;
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
                            return std::make_tuple(
                                       -a.time,
                                       countBits(a.brands & ~previous.matchBrands),
                                       countBits(a.brands), a.count) >
                                   std::make_tuple(
                                       -b.time,
                                       countBits(b.brands & ~previous.matchBrands),
                                       countBits(b.brands), b.count);
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
            if (optimistic <= scoreOf(result.finalState)) return;
            if (index == patrols.size()) {
                // A high-pickup column is provisional until every patrol has
                // a route. The full replay decides whether brand-repair
                // columns are needed; never accept a brand regression.
                const auto checked = simulateDay(setting, matchState, combined, previous);
                if (checked.valid && scoreOf(checked.state) > scoreOf(result.finalState) &&
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
