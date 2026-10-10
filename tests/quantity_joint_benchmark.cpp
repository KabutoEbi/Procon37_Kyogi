#include "model.hpp"
#include "solver_options.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

// Paired random 16x16 matches. All options except the joint-route feature
// match the production defaults. time-ms=0 fixes beam widths with a 5-second
// watchdog for brand completion on difficult random maps.
int main(int argc, char** argv) {
    using namespace benzen;
    try {
        if (argc < 5 || argc > 7) {
            std::cerr << "usage: quantity_joint_benchmark seed-start count time-ms mixed|tight|smooth [lead|middle|long] [last-day]\n";
            return 2;
        }
        const int first = std::stoi(argv[1]), count = std::stoi(argv[2]);
        const int timeMs = std::stoi(argv[3]);
        const std::string traffic = argv[4];
        const std::string role = argc >= 6 ? argv[5] : "normal";
        const bool lastDay = argc == 7 && std::string(argv[6]) == "last-day";
        if (first < 1 || count < 1 || count > 1000 || first > 1000000 ||
            timeMs < 0 || (traffic != "mixed" && traffic != "tight" && traffic != "smooth") ||
            (role != "normal" && role != "lead" && role != "middle" && role != "long"))
            throw std::invalid_argument("invalid benchmark arguments");
        if (argc == 7 && !lastDay) throw std::invalid_argument("expected last-day");
        using Score = std::tuple<int, int, int>;
        std::array<std::int64_t, 3> totalsOld{}, totalsNew{};
        int wins = 0, ties = 0, losses = 0;
        std::cout << "seed,variant,day,match_brands,daily_brands,udon,ms,route_attempts,route_improvements\n";
        for (int seed = first; seed < first + count; ++seed) {
            CreateConfig create;
            create.seed = seed;
            create.brandCount = 10;
            create.dayCount = 5;
            auto setting = ProblemCreator::generate(create).setting;
            setting.startsAt = 0;
            setting.daySteps = {32, 36, 40, 40, 48};
            if (lastDay) {
                setting.daySteps = {48};
                setting.daySeconds.resize(1);
            }
            setting.fuelLimit = traffic == "tight" ? 20 : 40;
            for (std::size_t i = 0; i < setting.field.spots.size(); ++i)
                setting.field.spots[i].maxStock = i < 14 ? 2 : 1;
            std::array<Score, 2> scores;
            // Alternate execution order to reduce first-run/cache bias.
            for (int run = 0; run < 2; ++run) {
                const int variant = (seed + run) % 2;
                MatchRunnerConfig config;
                config.beam.spotRouteLns.trafficAwareJointRoutes = variant != 0;
                config.beam.timeLimitMs = timeMs == 0 ? 5000 : timeMs;
                config.beam.safetyMarginMs = 0;
                config.beam.useStateDeadline = false;
                config.beam.threadCount = 1;
                if (timeMs == 0) {
                    // Deterministic hill-climbing used by roles 1 and 2. A
                    // finite deadline would otherwise expand the SA iteration
                    // count and turn the fixed-width case into a different test.
                    config.beam.udonAlns.simulatedAnnealing = false;
                    config.beam.beamWidth = 32;
                    config.beam.jointBeamWidth = 128;
                    config.beam.adaptiveBeamWidth = false;
                    config.beam.autoSizedBeam = false;
                } else {
                    config.beam.adaptiveBeamWidth = true;
                    config.beam.autoSizedBeam = true;
                }
                if (role != "normal") {
                    config.beam.udonAlns.simulatedAnnealing = role == "long";
                    config.beam.stopOnStableBeam = role == "middle";
                    config.beam.udonAlns.stopOnStagnation = role == "long";
                }
                // Timed isolated last-day runs use the same width calibration
                // as main.cpp's CLI, instead of the benchmark's fixed widths.
                if (lastDay && timeMs > 0) {
                    MatchState initial;
                    initial.agents.resize(create.agentCount);
                    SolverOptions options;
                    options.hasTimeLimit = true;
                    config.beam = options.beamFor(setting, initial, config.beam);
                }
                config.trafficByDay.assign(setting.dayCount(),
                    std::vector<TrafficStatus>(setting.field.cellCount(), TrafficStatus::Smooth));
                if (traffic != "smooth") {
                    std::uint64_t random = static_cast<std::uint64_t>(seed) ^ UINT64_C(0xa3b195354a39b70d);
                    for (auto& day : config.trafficByDay)
                        for (std::size_t pos = 0; pos < day.size(); ++pos) {
                            if (setting.field.at(static_cast<CellPos>(pos)).type != CellType::Road) continue;
                            const auto draw = brain2_detail::nextAnnealingRandom(random) % 10;
                            day[pos] = draw < 4 ? TrafficStatus::Smooth :
                                draw < 7 ? TrafficStatus::Busy : TrafficStatus::Jammed;
                        }
                }
                const std::vector<AgentKind> kinds{
                    AgentKind::Patrol, AgentKind::Patrol, AgentKind::Patrol, AgentKind::Supply};
                const auto match = runMatch(setting, kinds, config);
                if (!match.success) throw std::runtime_error("match failed");
                ScoreState previous;
                for (const auto& day : match.days) {
                    const auto replay = simulateDay(setting, day.startState, day.plan, previous);
                    if (!replay.valid || day.usedFallback || replay.state != day.endState)
                        throw std::runtime_error("invalid or inconsistent replay");
                    previous = day.endState.score;
                    std::cout << seed << ',' << (variant ? "new" : "old") << ',' << day.day << ','
                        << countBits(previous.matchBrands) << ',' << previous.sumOfDailyBrandCounts << ','
                        << previous.totalUdon << ',' << day.elapsedMs << ','
                        << day.spotRouteLnsAttempts << ',' << day.spotRouteLnsImprovements << '\n';
                }
                scores[variant] = {countBits(match.score.matchBrands),
                    match.score.sumOfDailyBrandCounts, match.score.totalUdon};
                auto& total = variant ? totalsNew : totalsOld;
                total[0] += std::get<0>(scores[variant]);
                total[1] += std::get<1>(scores[variant]);
                total[2] += std::get<2>(scores[variant]);
                std::cout.flush();
            }
            if (scores[1] > scores[0]) ++wins;
            else if (scores[1] < scores[0]) ++losses;
            else ++ties;
        }
        std::cerr << "old=" << totalsOld[0] << '/' << totalsOld[1] << '/' << totalsOld[2]
            << " new=" << totalsNew[0] << '/' << totalsNew[1] << '/' << totalsNew[2]
            << " wins/ties/losses=" << wins << '/' << ties << '/' << losses << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
