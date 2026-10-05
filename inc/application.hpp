#pragma once

#include "model.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace benzen {

struct ApplicationConfig {
    CreateConfig createConfig{};
    BeamSearchConfig beamConfig{};
    std::string outputPath = "problem.json";
    // -1ならエージェント種別選択器に任せる。
    std::int32_t supplyCount = -1;
};

namespace application_detail {

inline void validateCompetitionMap(const CreateConfig& config) {
    const bool roundOne = config.width == 16 && config.height == 16 &&
        config.agentCount == 4 && config.spotCount == 16 &&
        config.brandCount >= 10 && config.brandCount <= 14;
    const bool semifinal = config.width == 24 && config.height == 24 &&
        config.agentCount == 5 && config.spotCount == 24 &&
        config.brandCount >= 14 && config.brandCount <= 20;
    const bool final = config.width == 32 && config.height == 32 &&
        config.agentCount == 7 && config.spotCount == 32 &&
        config.brandCount >= 20 && config.brandCount <= 28;
    if ((!roundOne && !semifinal && !final) || config.daySeconds != 60) {
        throw std::invalid_argument(
            "map must be 16x16/4 agents/16 spots/10..14 brands, "
            "24x24/5 agents/24 spots/14..20 brands, or "
            "32x32/7 agents/32 spots/20..28 brands; "
            "answer seconds per day must be 60");
    }
}

template <class Integer>
Integer parseInteger(std::string_view text, std::string_view name) {
    Integer val{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [pos, err] = std::from_chars(begin, end, val);
    if (err != std::errc{} || pos != end) {
        throw std::invalid_argument(
            "invalid integer for " + std::string(name) + ": " +
            std::string(text));
    }
    return val;
}

inline std::string_view requireValue(
    int& i,
    int argc,
    char* argv[],
    std::string_view name) {
    if (i + 1 >= argc) {
        throw std::invalid_argument(
            "missing value for " + std::string(name));
    }
    return argv[++i];
}

inline ApplicationConfig parseOptions(
    int argc,
    char* argv[],
    bool allowPath,
    ApplicationConfig cfg) {
    bool pathUsed = false;

    for (int i = 0; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](std::string_view name) {
            return requireValue(i, argc, argv, name);
        };

        if (arg == "--output") {
            cfg.outputPath = value(arg);
            pathUsed = true;
        } else if (arg == "--width") {
            cfg.createConfig.width =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--height") {
            cfg.createConfig.height =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--agents") {
            cfg.createConfig.agentCount =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--spots") {
            cfg.createConfig.spotCount =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--brands") {
            cfg.createConfig.brandCount =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--days") {
            cfg.createConfig.dayCount =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--seconds") {
            cfg.createConfig.daySeconds =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--teams") {
            cfg.createConfig.teamCount =
                parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--seed") {
            cfg.createConfig.seed = parseInteger<std::uint64_t>(value(arg), arg);
        } else if (arg == "--supplies") {
            cfg.supplyCount = parseInteger<std::int32_t>(value(arg), arg);
        } else if (arg == "--beam-width") {
            cfg.beamConfig.beamWidth =
                parseInteger<std::size_t>(value(arg), arg);
        } else if (arg == "--joint-beam-width") {
            cfg.beamConfig.jointBeamWidth =
                parseInteger<std::size_t>(value(arg), arg);
        } else if (!arg.empty() && arg.front() != '-' &&
                   allowPath && !pathUsed) {
            cfg.outputPath = arg;
            pathUsed = true;
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
    }
    return cfg;
}

template <class Printer>
bool writeJsonFile(const std::string& path, Printer&& print) {
    std::ofstream output(path);
    return output && print(output) && static_cast<bool>(output);
}

inline void printCreatedProblem(
    std::ostream& output,
    const std::string& path,
    const GeneratedProblem& problem) {
    output << "created " << path
           << " (seed=" << problem.seed
           << ", map=" << problem.setting.field.width << 'x'
           << problem.setting.field.height
           << ", agents=" << problem.setting.agentCount()
           << ", spots=" << problem.setting.field.spots.size()
           << ", brands=" << problem.setting.field.brands.size()
           << ")\n";
}

template <class Writer>
int createProblem(
    int argc,
    char* argv[],
    const ApplicationConfig& defaults,
    Writer&& write) {
    const ApplicationConfig options = parseOptions(argc, argv, true, defaults);
    validateCompetitionMap(options.createConfig);
    const GeneratedProblem problem = ProblemCreator::generate(options.createConfig);
    if (!write(problem, options.outputPath)) {
        std::cerr << "failed to write: " << options.outputPath << '\n';
        return 1;
    }
    printCreatedProblem(std::cout, options.outputPath, problem);
    return 0;
}

}  // namespace application_detail

inline void printUsage(std::ostream& output, const char* program) {
    output
        << "Usage:\n"
        << "  " << program << " create [output.json] [options]\n"
        << "  " << program << " solve [options]\n\n"
        << "Problem options:\n"
        << "  --width N              16, 24 or 32 (square map only)\n"
        << "  --height N             same as width\n"
        << "  --agents N             4, 5 or 7 for the respective size\n"
        << "  --spots N              16, 24 or 32 for the respective size\n"
        << "  --brands N             10..14, 14..20 or 20..28\n"
        << "  --days N               number of days (4..10)\n"
        << "  --seconds N            answer seconds per day (60 only)\n"
        << "  --teams N              number of teams\n"
        << "  --seed N               deterministic random seed\n\n"
        << "Solve options:\n"
        << "  --supplies N           fixed supply count (default: auto)\n"
        << "  --beam-width N         main beam width\n"
        << "  --joint-beam-width N   joint action beam width\n";
}

inline MatchState makeInitialState(
    const MatchSetting& s,
    const std::vector<AgentKind>& kinds) {
    if (kinds.size() != s.agentCount()) {
        throw std::invalid_argument(
            "agent kind count must match agent starts");
    }

    MatchState st;
    st.trafficByPos.assign(s.field.cellCount(), TrafficStatus::Smooth);
    st.agents.reserve(s.agentCount());
    for (std::size_t i = 0; i < s.agentCount(); ++i) {
        st.agents.push_back(
            Agent{i, kinds[i], s.agentStarts[i], s.fuelLimit});
    }
    return st;
}

inline MatchState makeInitialState(
    const MatchSetting& s,
    std::int32_t supplyN) {
    if (supplyN < 0 || supplyN >= static_cast<std::int32_t>(s.agentCount())) {
        throw std::invalid_argument(
            "supplies must be in [0, agentCount - 1]");
    }

    std::vector<AgentKind> kinds(s.agentCount(), AgentKind::Patrol);
    const std::size_t first =
        s.agentCount() - static_cast<std::size_t>(supplyN);
    for (std::size_t i = first; i < s.agentCount(); ++i) {
        kinds[i] = AgentKind::Supply;
    }
    return makeInitialState(s, kinds);
}

inline DailyPlan makeWaitPlan(
    const MatchSetting& setting,
    const MatchState& state) {
    const std::int32_t steps =
        setting.daySteps[static_cast<std::size_t>(state.day)];
    return DailyPlan(state.agents.size(), AgentPlan{-steps});
}

inline BeamSearchResult solveDay(
    const MatchSetting& setting,
    const MatchState& state,
    ScoreState previousScore,
    const BeamSearchConfig& config) {
    BeamSearchResult result =
        brain1(setting, state, previousScore, config);
    if (result.success) {
        return result;
    }

    result.plan = makeWaitPlan(setting, state);
    const SimulationResult fallback =
        simulateDay(setting, state, result.plan, previousScore);
    if (fallback.valid) {
        result.success = true;
        result.finalState = fallback.state;
    }
    return result;
}

inline void printPlan(std::ostream& output, const DailyPlan& plan) {
    const DailyPlan compressed = compressWaitActions(plan);
    output << "[\n";
    for (std::size_t agent = 0; agent < compressed.size(); ++agent) {
        output << "  [";
        for (std::size_t action = 0;
             action < compressed[agent].size();
             ++action) {
            if (action != 0) {
                output << ", ";
            }
            output << compressed[agent][action];
        }
        output << "]"
               << (agent + 1 == compressed.size() ? "\n" : ",\n");
    }
    output << "]\n";
}

inline void printAgentKinds(
    std::ostream& output,
    const std::vector<AgentKind>& kinds) {
    output << '[';
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (i != 0) output << ", ";
        output << static_cast<std::int32_t>(kinds[i]);
    }
    output << "]\n";
}

inline bool writeAgentKindsJson(
    const std::vector<AgentKind>& kinds,
    const std::string& path = "agents.json") {
    if (kinds.empty() || std::any_of(
            kinds.begin(), kinds.end(), [](AgentKind kind) {
                return kind != AgentKind::Patrol && kind != AgentKind::Supply;
            })) {
        return false;
    }
    return application_detail::writeJsonFile(path, [&](std::ostream& output) {
        printAgentKinds(output, kinds);
        return true;
    });
}

inline void printScoreCheckpoint(
    std::ostream& output,
    const ScoreCheckpoint& checkpoint) {
    output << "{\n"
           << "  \"nextDay\": " << checkpoint.nextDay << ",\n"
           << "  \"matchBrands\": " << checkpoint.score.matchBrands << ",\n"
           << "  \"totalUdon\": " << checkpoint.score.totalUdon << ",\n"
           << "  \"sumOfDailyBrandCounts\": "
           << checkpoint.score.sumOfDailyBrandCounts << "\n"
           << "}\n";
}

inline bool writeScoreCheckpointJson(
    const ScoreCheckpoint& checkpoint,
    const std::string& path = "score-next.json") {
    if (checkpoint.nextDay < 0 || checkpoint.score.totalUdon < 0 ||
        checkpoint.score.sumOfDailyBrandCounts < 0 ||
        (checkpoint.score.matchBrands >> kMaxBrands) != 0) {
        return false;
    }
    return application_detail::writeJsonFile(path, [&](std::ostream& output) {
        printScoreCheckpoint(output, checkpoint);
        return true;
    });
}

// 探索終了時の表示に必要な自チーム状態とスポット残在庫を保存する。
inline bool printResultStateJson(
    std::ostream& output,
    const GameState& state) {
    if (state.day < 0 || state.agentCount == 0 ||
        state.agentCount > kMaxAgents || state.spotCount > kMaxSpots) {
        return false;
    }
    for (std::size_t i = 0; i < state.agentCount; ++i) {
        const Agent& agent = state.agents[i];
        if (agent.pos < 0 || agent.fuel < 0 ||
            (agent.kind != AgentKind::Patrol &&
             agent.kind != AgentKind::Supply)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < state.spotCount; ++i) {
        if (state.remainingStocks[i] < 0) return false;
    }

    output << "{\n"
           << "  \"day\": " << state.day << ",\n"
           << "  \"step\": " << state.step << ",\n"
           << "  \"agents\": [\n";
    for (std::size_t i = 0; i < state.agentCount; ++i) {
        const Agent& agent = state.agents[i];
        output << "    {\"kind\": " << static_cast<int>(agent.kind)
               << ", \"pos\": " << agent.pos
               << ", \"fuel\": " << agent.fuel << "}"
               << (i + 1 == state.agentCount ? "\n" : ",\n");
    }
    output << "  ],\n"
           << "  \"remainingStocks\": [";
    for (std::size_t i = 0; i < state.spotCount; ++i) {
        if (i != 0) output << ", ";
        output << state.remainingStocks[i];
    }
    output << "]\n}\n";
    return static_cast<bool>(output);
}

inline bool writeResultStateJson(
    const GameState& state,
    const std::string& path = "result-state.json") {
    return application_detail::writeJsonFile(path, [&](std::ostream& output) {
        return printResultStateJson(output, state);
    });
}

// POST / の本文と同じJSON配列をファイルへ保存する。
inline bool writePlanJson(
    const DailyPlan& plan,
    const std::string& path = "answer.json") {
    return application_detail::writeJsonFile(path, [&](std::ostream& output) {
        printPlan(output, plan);
        return true;
    });
}

inline int create(
    int argc,
    char* argv[],
    const ApplicationConfig& defaults = {}) {
    return application_detail::createProblem(
        argc, argv, defaults,
        [](const GeneratedProblem& problem, const std::string& path) {
            return ProblemCreator::writeJson(problem, path);
        });
}

// GUIやローカル探索で直接読み込めるGET /setting形式を生成する。
inline int createMatchSetting(
    int argc,
    char* argv[],
    const ApplicationConfig& defaults = {}) {
    return application_detail::createProblem(
        argc, argv, defaults,
        [](const GeneratedProblem& problem, const std::string& path) {
            return ProblemCreator::writeMatchSettingJson(problem.setting, path);
        });
}

inline int runLocalSolver(
    int argc,
    char* argv[],
    const ApplicationConfig& defaults = {}) {
    const ApplicationConfig options = application_detail::parseOptions(
        argc, argv, false, defaults);
    application_detail::validateCompetitionMap(options.createConfig);
    const GeneratedProblem problem =
        ProblemCreator::generate(options.createConfig);
    MatchState state;
    if (options.supplyCount < 0) {
        const AgentSelectionResult selection =
            selectAgentKinds(problem.setting);
        if (!selection.success) {
            std::cerr << "failed to select agent kinds\n";
            return 1;
        }
        state = makeInitialState(problem.setting, selection.kinds);
    } else {
        state = makeInitialState(problem.setting, options.supplyCount);
    }
    const BeamSearchResult result = solveDay(
        problem.setting,
        state,
        {},
        options.beamConfig);
    if (!result.success) {
        std::cerr << "failed to construct a valid plan\n";
        return 1;
    }

    printPlan(std::cout, result.plan);
    std::cerr << "expanded=" << result.expandedNodes
              << " full-eval=" << result.fullEvaluations
              << " reused-eval=" << result.reusedEvaluations
              << " score=" << result.evaluation.total
              << " udon=" << result.finalState.dayUdon << '\n';
    return 0;
}

inline int runApplication(
    int argc,
    char* argv[],
    const ApplicationConfig& defaults = {}) {
    if (argc < 2) {
        printUsage(std::cerr, argv[0]);
        return 1;
    }

    try {
        const std::string_view command = argv[1];
        if (command == "create") {
            return create(argc - 2, argv + 2, defaults);
        }
        if (command == "solve") {
            return runLocalSolver(argc - 2, argv + 2, defaults);
        }

        printUsage(std::cerr, argv[0]);
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace benzen
