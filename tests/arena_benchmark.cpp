#include "application.hpp"
#include "json_input.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<benzen::AgentKind> kindsFor(
    std::size_t agentCount,
    std::uint64_t supplyMask) {
    if ((supplyMask >> agentCount) != 0 ||
        supplyMask + 1 == (UINT64_C(1) << agentCount)) {
        throw std::invalid_argument(
            "supply mask must not select all agents");
    }
    std::vector<benzen::AgentKind> kinds(
        agentCount, benzen::AgentKind::Patrol);
    for (std::size_t i = 0; i < agentCount; ++i) {
        if ((supplyMask & (UINT64_C(1) << i)) != 0) {
            kinds[i] = benzen::AgentKind::Supply;
        }
    }
    return kinds;
}

void addMixedTraffic(
    const benzen::MatchSetting& setting,
    benzen::MatchRunnerConfig& config) {
    config.trafficByDay.assign(
        setting.dayCount(),
        std::vector<benzen::TrafficStatus>(
            setting.field.cellCount(), benzen::TrafficStatus::Smooth));
    for (std::size_t day = 0; day < setting.dayCount(); ++day) {
        for (std::size_t pos = 0; pos < setting.field.cellCount(); ++pos) {
            if (setting.field.at(static_cast<benzen::CellPos>(pos)).type !=
                benzen::CellType::Road) {
                continue;
            }
            const std::uint64_t hash =
                (pos + 1) * UINT64_C(0x9e3779b97f4a7c15) ^
                (day + 11) * UINT64_C(0xbf58476d1ce4e5b9);
            const unsigned bucket = static_cast<unsigned>((hash >> 32) % 20);
            if (bucket < 2) {
                config.trafficByDay[day][pos] = benzen::TrafficStatus::Jammed;
            } else if (bucket < 6) {
                config.trafficByDay[day][pos] = benzen::TrafficStatus::Busy;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace benzen;
    try {
        if (argc < 2 || argc > 13) {
            std::cerr << "usage: " << argv[0]
                      << " setting.json [time-ms] [supply-mask] [traffic]"
                         " [beam-width] [joint-beam-width] [hard-next-day]"
                         " [details] [arena-guidance] [joint-destroy]"
                         " [detour-aware-spots] [differential-insertion]\n";
            return 2;
        }
        MatchSetting setting;
        if (std::string(argv[1]) == "-") {
            const std::string json{
                std::istreambuf_iterator<char>(std::cin),
                std::istreambuf_iterator<char>()};
            setting = parseMatchSettingJson(json);
        } else {
            setting = readMatchSettingJson(argv[1]);
        }
        // 公開済みの過去試合でも、オフライン測定では締切を相対時間にする。
        setting.startsAt = 0;

        const int timeMs = argc >= 3 ? std::stoi(argv[2]) : 1000;
        const int supplyMask = argc >= 4 ? std::stoi(argv[3]) : 1;
        if (timeMs <= 0 || supplyMask < 0) {
            throw std::invalid_argument(
                "time must be positive and supply mask must be non-negative");
        }

        MatchRunnerConfig config;
        config.beam.timeLimitMs = timeMs;
        config.beam.safetyMarginMs = 0;
        config.beam.useStateDeadline = false;
        if (argc >= 5 && std::string(argv[4]) == "mixed") {
            addMixedTraffic(setting, config);
        } else if (argc >= 5 && std::string(argv[4]) != "smooth") {
            throw std::invalid_argument("traffic mode must be 'smooth' or 'mixed'");
        }
        if (argc >= 6) config.beam.beamWidth = std::stoul(argv[5]);
        if (argc >= 7) config.beam.jointBeamWidth = std::stoul(argv[6]);
        if (argc >= 8) {
            const int enabled = std::stoi(argv[7]);
            if (enabled < 0 || enabled > 1) {
                throw std::invalid_argument("hard-next-day must be 0 or 1");
            }
            config.beam.prioritizeHardNextDayTargets = enabled != 0;
        }
        const bool details = argc >= 9 && std::stoi(argv[8]) != 0;
        if (argc >= 10) {
            const int enabled = std::stoi(argv[9]);
            if (enabled < 0 || enabled > 3) {
                throw std::invalid_argument(
                    "arena-guidance must be 0..3");
            }
            config.beam.udonAlns.arenaGuidedNeighborhoods =
                (enabled & 1) != 0;
            config.beam.udonAlns.stockWeightedSpotSelection =
                (enabled & 2) != 0;
        }
        if (argc >= 11) {
            const int enabled = std::stoi(argv[10]);
            if (enabled < 0 || enabled > 1) {
                throw std::invalid_argument(
                    "joint-destroy must be 0 or 1");
            }
            config.beam.udonAlns.jointDestroyRepair = enabled != 0;
        }
        if (argc >= 12) {
            const int enabled = std::stoi(argv[11]);
            if (enabled < 0 || enabled > 1) {
                throw std::invalid_argument(
                    "detour-aware-spots must be 0 or 1");
            }
            config.beam.udonAlns.detourAwareSpotSelection = enabled != 0;
        }
        if (argc >= 13) {
            const int enabled = std::stoi(argv[12]);
            if (enabled < 0 || enabled > 1) {
                throw std::invalid_argument(
                    "differential-insertion must be 0 or 1");
            }
            config.beam.udonAlns.differentialInsertionEvaluation =
                enabled != 0;
        }
        const auto match = runMatch(
            setting,
            kindsFor(setting.agentCount(), static_cast<std::uint64_t>(supplyMask)),
            config);
        if (!match.success) {
            std::cerr << "failed on day " << match.failedDay << '\n';
            return 1;
        }
        std::cout << "day,kinds,daily_kinds,balls,day_mask,ms,nodes\n";
        for (const auto& day : match.days) {
            std::cout << day.day << ','
                      << countBits(day.endState.score.matchBrands) << ','
                      << day.endState.score.sumOfDailyBrandCounts << ','
                      << day.endState.score.totalUdon << ','
                      << day.endState.dayBrands << ','
                      << day.elapsedMs << ',' << day.expandedNodes << '\n';
            if (details) {
                std::cerr << "day " << day.day << " start";
                for (const auto& agent : day.startState.agents) {
                    std::cerr << ' ' << (agent.isSupply() ? 'S' : 'P')
                              << agent.pos << '/' << agent.fuel;
                }
                std::cerr << " end";
                for (std::size_t i = 0; i < day.endState.agentCount; ++i) {
                    const auto& agent = day.endState.agents[i];
                    std::cerr << ' ' << (agent.isSupply() ? 'S' : 'P')
                              << agent.pos << '/' << agent.fuel;
                }
                std::cerr << '\n';
                std::cerr << "  refine spot=" << day.spotRouteLnsAttempts
                          << '/' << day.spotRouteLnsImprovements
                          << " portfolio=" << day.spotPortfolioMaxBrands
                          << '/' << day.spotPortfolioMaxUdon
                          << '/' << day.spotPortfolioMaxValidUdon
                          << " columns=" << day.routeColumnsGenerated
                          << '/' << day.routeColumnsImprovements
                          << " alns=" << day.udonAlnsAttempts
                          << '/' << day.udonAlnsImprovements << '\n';
            }
        }
        std::cout << "result=" << countBits(match.score.matchBrands) << '/'
                  << match.score.sumOfDailyBrandCounts << '/'
                  << match.score.totalUdon << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
