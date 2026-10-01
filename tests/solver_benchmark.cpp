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
    bool currentOnly = false, noCompletion = false, legacyWeights = false, tightFuel = false;
    bool singleDay = false, mixedTraffic = false, fixedWork = false, profile = false;
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
            else throw std::invalid_argument("unknown option");
        }
        if (seedStart > std::numeric_limits<int>::max() - seeds)
            throw std::invalid_argument("seed range overflow");
        if (sizeOnly != 0 && sizeOnly != 8 && sizeOnly != 16 && sizeOnly != 24 && sizeOnly != 32)
            throw std::invalid_argument("invalid size");
        const std::vector<std::string> allowed = {"improved", "no-cache", "no-coordination", "no-detour", "no-lookahead", "no-macros"};
        if (std::find(allowed.begin(), allowed.end(), variant) == allowed.end())
            throw std::invalid_argument("invalid variant");
        struct Summary {
            std::int64_t brands = 0, daily = 0, udon = 0, nodes = 0;
            std::vector<std::int64_t> times;
        };
        std::map<std::string, Summary> summaries;
        using Score = std::tuple<int, int, int>;
        int wins = 0, ties = 0, losses = 0;
        std::cout << "size,seed,repeat,variant,day,brands,daily_brands,udon,ms,nodes,cache_hits,assignment_computations,valid,plan_hash,assignment_ns,flow_ns,rendezvous_ns\n";
        for (const int size : {8, 16, 24, 32}) {
            if (sizeOnly != 0 && size != sizeOnly) continue;
            for (int seed = seedStart; seed - seedStart < seeds; ++seed) {
                CreateConfig create;
                create.width = create.height = size;
                create.agentCount = size <= 16 ? 4 : 8;
                create.spotCount = size;
                create.brandCount = size * 3 / 4;
                create.seed = seed;
                auto setting = ProblemCreator::generate(create).setting;
                if (tightFuel) setting.fuelLimit = std::max(1, setting.daySteps.front() / 2);
                if (singleDay) { setting.daySteps.resize(1); setting.daySeconds.resize(1); }
                std::vector<AgentKind> kinds(create.agentCount, AgentKind::Patrol);
                kinds.back() = AgentKind::Supply;
                for (int repeat = 0; repeat < repeats; ++repeat) {
                    std::vector<std::string> order{"baseline", variant};
                    if (currentOnly) order = {variant};
                    if ((seed + repeat) % 2) std::reverse(order.begin(), order.end());
                    std::map<std::string, Score> scores;
                    for (const auto& name : order) {
                        MatchRunnerConfig config;
                        auto& beam = config.beam;
                        beam.coordinatedAssignments = name != "baseline" && name != "no-coordination";
                        beam.detourRendezvous = name != "baseline" && name != "no-detour";
                        beam.twoSpotLookahead = name != "baseline" && name != "no-lookahead";
                        beam.assignmentCacheLimit = name == "baseline" || name == "no-cache" ? 0 : 4096;
                        beam.useMacroActions = name != "no-macros";
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
                        for (const auto& day : match.days) {
                            const auto check = simulateDay(setting, day.startState, day.plan, previous);
                            const bool valid = check.valid && !day.usedFallback && check.state == day.endState;
                            std::uint64_t fingerprint = UINT64_C(1469598103934665603);
                            for (const auto& agent : day.plan) {
                                fingerprint = (fingerprint ^ agent.size()) * UINT64_C(1099511628211);
                                for (const auto action : agent)
                                    fingerprint = (fingerprint ^ static_cast<std::uint32_t>(action)) * UINT64_C(1099511628211);
                            }
                            std::cout << size << ',' << seed << ',' << repeat << ',' << name << ',' << day.day << ','
                                << countBits(day.endState.score.matchBrands) << ',' << day.endState.score.sumOfDailyBrandCounts << ','
                                << day.endState.score.totalUdon << ',' << day.elapsedMs << ',' << day.expandedNodes << ','
                                << day.assignmentCacheHits << ',' << day.assignmentComputations << ',' << valid << ','
                                << fingerprint << ',' << day.assignmentTimeNs << ',' << day.flowTimeNs << ','
                                << day.rendezvousTimeNs << '\n';
                            if (!valid) return 1;
                            previous = day.endState.score;
                            elapsed += day.elapsedMs;
                            nodes += day.expandedNodes;
                        }
                        auto& summary = summaries[name];
                        const int brands = countBits(match.score.matchBrands);
                        summary.brands += brands;
                        summary.daily += match.score.sumOfDailyBrandCounts;
                        summary.udon += match.score.totalUdon;
                        summary.nodes += nodes;
                        summary.times.push_back(elapsed);
                        scores[name] = {brands, match.score.sumOfDailyBrandCounts, match.score.totalUdon};
                    }
                    if (currentOnly) continue;
                    if (scores[variant] > scores["baseline"]) ++wins;
                    else if (scores[variant] < scores["baseline"]) ++losses;
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
                << " max_ms=" << summary.times.back() << " nodes=" << summary.nodes << '\n';
        }
        if (!currentOnly)
            std::cerr << variant << " vs baseline wins/ties/losses=" << wins << '/' << ties << '/' << losses << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
