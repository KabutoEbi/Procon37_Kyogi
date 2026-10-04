#include "application.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

int main(int argc, char** argv) {
    using namespace benzen;
    int seeds = 8, repeats = 3, timeMs = 300, sizeOnly = 0, seedStart = 1;
    int agentOnly = 0, spotOnly = 0, brandOnly = 0, dayOnly = 0;
    bool currentOnly = false, noCompletion = false, legacyWeights = false, tightFuel = false;
    bool singleDay = false, mixedTraffic = false, fixedWork = false, profile = false;
    bool compareBrandRepair = false, compareUdonAlns = false;
    std::string variant = "improved";
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--legacy-weights") { legacyWeights = true; continue; }
            if (option == "--tight-fuel") { tightFuel = true; continue; }
            if (option == "--no-completion") { noCompletion = true; continue; }
            if (option == "--current-only") { currentOnly = true; continue; }
            if (option == "--single-day") { singleDay = true; continue; }
            if (option == "--mixed-traffic") { mixedTraffic = true; continue; }
            if (option == "--fixed-work") { fixedWork = true; continue; }
            if (option == "--profile") { profile = true; continue; }
            if (option == "--compare-brand-repair") { compareBrandRepair = true; continue; }
            if (option == "--compare-udon-alns") { compareUdonAlns = true; continue; }
            if (++i == argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[i];
            if (option == "--variant") { variant = value; continue; }
            std::size_t consumed = 0;
            const int number = std::stoi(value, &consumed);
            if (consumed != value.size() || number <= 0)
                throw std::invalid_argument("expected a positive integer");
            if (option == "--seeds") seeds = number;
            else if (option == "--seed-start") seedStart = number;
            else if (option == "--repeats") repeats = number;
            else if (option == "--time-ms") timeMs = number;
            else if (option == "--size") sizeOnly = number;
            else if (option == "--agents") agentOnly = number;
            else if (option == "--spots") spotOnly = number;
            else if (option == "--brands") brandOnly = number;
            else if (option == "--days") dayOnly = number;
            else throw std::invalid_argument("unknown option");
        }
        if (seedStart > std::numeric_limits<int>::max() - seeds)
            throw std::invalid_argument("seed range overflow");
        if (sizeOnly != 0 && sizeOnly != 8 && sizeOnly != 16 && sizeOnly != 24 && sizeOnly != 32)
            throw std::invalid_argument("invalid size");
        if ((agentOnly != 0 && (agentOnly < 3 || agentOnly > 8)) ||
            (dayOnly != 0 && (dayOnly < 4 || dayOnly > 10)) ||
            spotOnly < 0 || brandOnly < 0 ||
            (spotOnly > 0 && brandOnly > spotOnly))
            throw std::invalid_argument("invalid generated setting override");
        if (compareBrandRepair && compareUdonAlns)
            throw std::invalid_argument("select one paired comparison");
        if (compareBrandRepair) {
            if (currentOnly) throw std::invalid_argument("paired comparison requires both variants");
            variant = "brand-repair";
        }
        if (compareUdonAlns) {
            if (currentOnly) throw std::invalid_argument("paired comparison requires both variants");
            variant = "udon-alns";
        }
        const std::string baselineName = compareBrandRepair ? "no-brand-repair" :
            (compareUdonAlns ? "no-udon-alns" : "baseline");
        const std::vector<std::string> allowed = {"improved", "no-quantity-upgrades", "no-robust-route", "no-shared-rendezvous", "no-quantity-diversity", "no-cache", "no-coordination", "no-detour", "no-lookahead", "no-macros", "no-patrol-replan", "no-supply-cleanup", "wandering-supply", "limited-detour-scan", "no-hard-nextday", "no-route-portfolio", "replan-2", "replan-4", "replan-8", "detour-8", "final-detour-6", "lookahead-8", "full-actions", "brand-repair", "no-brand-repair", "udon-alns", "no-udon-alns"};
        if (std::find(allowed.begin(), allowed.end(), variant) == allowed.end())
            throw std::invalid_argument("invalid variant");
        struct Summary {
            std::int64_t brands = 0, daily = 0, udon = 0, nodes = 0;
            std::int64_t supplyBacktracks = 0;
            std::int64_t supplyMoves = 0;
            std::int64_t removedSupplyLoops = 0;
            std::int64_t repairAttempts = 0, repairExpanded = 0, repairImprovements = 0;
            std::int64_t alnsAttempts = 0, alnsExpanded = 0, alnsImprovements = 0;
            std::vector<std::int64_t> times;
        };
        std::map<std::string, Summary> summaries;
        using Score = std::tuple<int, int, int>;
        int wins = 0, ties = 0, losses = 0;
        std::cout << "size,seed,repeat,variant,day,brands,daily_brands,udon,ms,nodes,cache_hits,assignment_computations,supply_moves,supply_backtracks,removed_supply_loops,valid,plan_hash,assignment_ns,flow_ns,rendezvous_ns,repair_attempts,repair_expanded,repair_improvements,alns_attempts,alns_expanded,alns_improvements\n";
        for (const int size : {8, 16, 24, 32}) {
            if (sizeOnly != 0 && size != sizeOnly) continue;
            for (int seed = seedStart; seed - seedStart < seeds; ++seed) {
                CreateConfig create;
                create.width = create.height = size;
                create.agentCount = agentOnly > 0
                    ? agentOnly : (size <= 16 ? 4 : 8);
                create.spotCount = spotOnly > 0 ? spotOnly : size;
                create.brandCount = brandOnly > 0
                    ? brandOnly : size * 3 / 4;
                if (dayOnly > 0) create.dayCount = dayOnly;
                create.seed = seed;
                auto setting = ProblemCreator::generate(create).setting;
                if (tightFuel) setting.fuelLimit = std::max(1, setting.daySteps.front() / 2);
                if (singleDay) { setting.daySteps.resize(1); setting.daySeconds.resize(1); }
                std::vector<AgentKind> kinds(create.agentCount, AgentKind::Patrol);
                kinds.back() = AgentKind::Supply;
                for (int repeat = 0; repeat < repeats; ++repeat) {
                    std::vector<std::string> order{baselineName, variant};
                    if (currentOnly) order = {variant};
                    if ((seed + repeat) % 2) std::reverse(order.begin(), order.end());
                    std::map<std::string, Score> scores;
                    for (const auto& name : order) {
                        MatchRunnerConfig config;
                        auto& beam = config.beam;
                        beam.useBrandRepair = name != "no-brand-repair" && name != "baseline";
                        beam.useUdonAlns = name != "no-udon-alns" && name != "baseline";
                        beam.coordinatedAssignments = name != "baseline" && name != "no-coordination";
                        beam.detourRendezvous = name != "baseline" && name != "no-detour";
                        beam.exhaustFeasibleDetours = name != "limited-detour-scan";
                        beam.twoSpotLookahead = name != "baseline" && name != "no-lookahead";
                        beam.robustRouteLookahead = name != "no-quantity-upgrades" &&
                            name != "no-robust-route";
                        beam.sharedRendezvous = name != "no-quantity-upgrades" &&
                            name != "no-shared-rendezvous";
                        beam.diversifyAcquiredSpots = name != "no-quantity-upgrades" &&
                            name != "no-quantity-diversity";
                        beam.prioritizeHardNextDayTargets =
                            name != "baseline" && name != "no-hard-nextday";
                        beam.assignmentCacheLimit = name == "baseline" || name == "no-cache" ? 0 : 4096;
                        beam.useMacroActions = name != "no-macros";
                        beam.reassignStalePatrolRoutes =
                            name != "baseline" && name != "no-patrol-replan";
                        beam.simplifySupplyRoutes = name != "no-supply-cleanup";
                        beam.allowUnassignedSupplyMoves = name == "wandering-supply";
                        beam.comparePeriodicReplanPlans =
                            name != "baseline" && name != "no-route-portfolio";
                        if (name == "replan-2") beam.macroReplanInterval = 2;
                        if (name == "replan-4") beam.macroReplanInterval = 4;
                        if (name == "replan-8") beam.macroReplanInterval = 8;
                        if (name == "detour-8") beam.detourCandidateLimit = 8;
                        if (name == "final-detour-6")
                            beam.finalDayDetourCandidateLimit = 6;
                        if (name == "lookahead-8") beam.lookaheadCandidateLimit = 8;
                        if (name == "full-actions") beam.adaptiveActionLimit = false;
                        beam.completeIncumbentRoutes = !noCompletion;
                        if (legacyWeights) {
                            beam.fuelAwareWeights = false;
                        }
                        beam.threadCount = 1;
                        beam.timeLimitMs = timeMs;
                        beam.safetyMarginMs = 0;
                        beam.useStateDeadline = false;
                        beam.profilePlanning = profile;
                        if (fixedWork) {
                            beam.timeLimitMs = 0;
                            beam.adaptiveBeamWidth = false;
                            beam.beamWidth = 32;
                            beam.jointBeamWidth = 128;
                        }
                        if (mixedTraffic) {
                            config.trafficByDay.assign(setting.dayCount(),
                                std::vector<TrafficStatus>(setting.field.cellCount()));
                            for (std::size_t d = 0; d < setting.dayCount(); ++d)
                                for (std::size_t c = 0; c < setting.field.cellCount(); ++c)
                                    config.trafficByDay[d][c] = static_cast<TrafficStatus>((c + seed + d) % 3);
                        }
                        const auto match = runMatch(setting, kinds, config);
                        if (!match.success) return 1;
                        ScoreState previous;
                        std::int64_t elapsed = 0, nodes = 0;
                        std::int64_t matchSupplyMoves = 0;
                        std::int64_t matchSupplyBacktracks = 0;
                        for (const auto& day : match.days) {
                            const auto check = simulateDay(setting, day.startState, day.plan, previous);
                            const bool valid = check.valid && !day.usedFallback && check.state == day.endState;
                            std::uint64_t fingerprint = UINT64_C(1469598103934665603);
                            std::size_t supplyMoves = 0;
                            std::size_t supplyBacktracks = 0;
                            for (AgentIndex agent = 0;
                                 agent < day.plan.size(); ++agent) {
                                if (!day.startState.agents[agent].isSupply()) continue;
                                int previous = -1;
                                for (const int action : day.plan[agent]) {
                                    if (!isMoveAction(action)) continue;
                                    ++supplyMoves;
                                    if (previous >= 0 &&
                                        action == (previous + 3) % 6) {
                                        ++supplyBacktracks;
                                    }
                                    previous = action;
                                }
                            }
                            for (const auto& agent : day.plan) {
                                fingerprint = (fingerprint ^ agent.size()) * UINT64_C(1099511628211);
                                for (const auto action : agent)
                                    fingerprint = (fingerprint ^ static_cast<std::uint32_t>(action)) * UINT64_C(1099511628211);
                            }
                            std::cout << size << ',' << seed << ',' << repeat << ',' << name << ',' << day.day << ','
                                << countBits(day.endState.score.matchBrands) << ',' << day.endState.score.sumOfDailyBrandCounts << ','
                                << day.endState.score.totalUdon << ',' << day.elapsedMs << ',' << day.expandedNodes << ','
                                << day.assignmentCacheHits << ',' << day.assignmentComputations << ','
                                << supplyMoves << ',' << supplyBacktracks << ','
                                << day.removedSupplyLoops << ','
                                << valid << ','
                                << fingerprint << ',' << day.assignmentTimeNs << ',' << day.flowTimeNs << ','
                                << day.rendezvousTimeNs << ',' << day.brandRepairAttempts << ','
                                << day.brandRepairExpanded << ',' << day.brandRepairImprovements << ','
                                << day.udonAlnsAttempts << ',' << day.udonAlnsExpanded << ','
                                << day.udonAlnsImprovements << '\n';
                            if (!valid) return 1;
                            previous = day.endState.score;
                            elapsed += day.elapsedMs;
                            nodes += day.expandedNodes;
                            matchSupplyMoves += supplyMoves;
                            matchSupplyBacktracks += supplyBacktracks;
                        }
                        auto& summary = summaries[name];
                        const int brands = countBits(match.score.matchBrands);
                        summary.brands += brands;
                        summary.daily += match.score.sumOfDailyBrandCounts;
                        summary.udon += match.score.totalUdon;
                        summary.nodes += nodes;
                        summary.supplyMoves += matchSupplyMoves;
                        summary.supplyBacktracks += matchSupplyBacktracks;
                        for (const auto& day : match.days)
                            summary.removedSupplyLoops += day.removedSupplyLoops;
                        for (const auto& day : match.days) {
                            summary.repairAttempts += day.brandRepairAttempts;
                            summary.repairExpanded += day.brandRepairExpanded;
                            summary.repairImprovements += day.brandRepairImprovements;
                            summary.alnsAttempts += day.udonAlnsAttempts;
                            summary.alnsExpanded += day.udonAlnsExpanded;
                            summary.alnsImprovements += day.udonAlnsImprovements;
                        }
                        summary.times.push_back(elapsed);
                        scores[name] = {brands, match.score.sumOfDailyBrandCounts, match.score.totalUdon};
                    }
                    if (currentOnly) continue;
                    if (scores[variant] > scores[baselineName]) ++wins;
                    else if (scores[variant] < scores[baselineName]) ++losses;
                    else ++ties;
                }
            }
        }
        for (auto& entry : summaries) {
            auto& summary = entry.second;
            std::sort(summary.times.begin(), summary.times.end());
            std::cerr << entry.first << ": matches=" << summary.times.size()
                << " brands/daily/udon=" << summary.brands << '/' << summary.daily << '/' << summary.udon
                << " median_ms=" << summary.times[summary.times.size() / 2]
                << " max_ms=" << summary.times.back() << " nodes=" << summary.nodes
                << " supply_moves=" << summary.supplyMoves
                << " supply_backtracks=" << summary.supplyBacktracks
                << " removed_supply_loops=" << summary.removedSupplyLoops
                << " repair_attempts=" << summary.repairAttempts
                << " repair_expanded=" << summary.repairExpanded
                << " repair_improvements=" << summary.repairImprovements
                << " alns_attempts=" << summary.alnsAttempts
                << " alns_expanded=" << summary.alnsExpanded
                << " alns_improvements=" << summary.alnsImprovements << '\n';
        }
        if (!currentOnly)
            std::cerr << variant << " vs " << baselineName << " wins/ties/losses=" << wins << '/' << ties << '/' << losses << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
