#pragma once

#include "model.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace procon37 {

struct ApplicationConfig {
    CreateConfig createConfig{};
    BeamSearchConfig beamConfig{};
    std::string outputPath = "problem.json";
    // -1ならエージェント種別選択器に任せる。
    std::int32_t supplyCount = -1;
};

namespace application_detail {

template <class Integer>
Integer parseInteger(std::string_view text, std::string_view optionName) {
    Integer value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || position != end) {
        throw std::invalid_argument(
            "invalid integer for " + std::string(optionName) + ": " +
            std::string(text));
    }
    return value;
}

inline std::string_view requireValue(
    int& index,
    int argc,
    char* argv[],
    std::string_view optionName) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(
            "missing value for " + std::string(optionName));
    }
    ++index;
    return argv[index];
}

inline ApplicationConfig parseOptions(
    int argc,
    char* argv[],
    bool allowOutputPath,
    ApplicationConfig options) {
    bool positionalOutputUsed = false;

    for (int i = 0; i < argc; ++i) {
        const std::string_view argument = argv[i];
        auto value = [&](std::string_view name) {
            return requireValue(i, argc, argv, name);
        };

        if (argument == "--output") {
            options.outputPath = value(argument);
            positionalOutputUsed = true;
        } else if (argument == "--width") {
            options.createConfig.width =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--height") {
            options.createConfig.height =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--agents") {
            options.createConfig.agentCount =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--spots") {
            options.createConfig.spotCount =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--brands") {
            options.createConfig.brandCount =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--days") {
            options.createConfig.dayCount =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--seconds") {
            options.createConfig.daySeconds =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--teams") {
            options.createConfig.teamCount =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--seed") {
            options.createConfig.seed =
                parseInteger<std::uint64_t>(value(argument), argument);
        } else if (argument == "--supplies") {
            options.supplyCount =
                parseInteger<std::int32_t>(value(argument), argument);
        } else if (argument == "--beam-width") {
            options.beamConfig.beamWidth =
                parseInteger<std::size_t>(value(argument), argument);
        } else if (argument == "--joint-beam-width") {
            options.beamConfig.jointBeamWidth =
                parseInteger<std::size_t>(value(argument), argument);
        } else if (!argument.empty() && argument.front() != '-' &&
                   allowOutputPath && !positionalOutputUsed) {
            options.outputPath = argument;
            positionalOutputUsed = true;
        } else {
            throw std::invalid_argument(
                "unknown argument: " + std::string(argument));
        }
    }
    return options;
}

}  // namespace application_detail

inline void printUsage(std::ostream& output, const char* program) {
    output
        << "Usage:\n"
        << "  " << program << " create [output.json] [options]\n"
        << "  " << program << " solve [options]\n\n"
        << "Problem options:\n"
        << "  --width N              map width (8..32)\n"
        << "  --height N             map height (8..32)\n"
        << "  --agents N             agents per team (3..8)\n"
        << "  --spots N              number of spots\n"
        << "  --brands N             number of brands\n"
        << "  --days N               number of days (4..10)\n"
        << "  --seconds N            answer seconds per day\n"
        << "  --teams N              number of teams\n"
        << "  --seed N               deterministic random seed\n\n"
        << "Solve options:\n"
        << "  --supplies N           fixed supply count (default: auto)\n"
        << "  --beam-width N         main beam width\n"
        << "  --joint-beam-width N   joint action beam width\n";
}

inline MatchState makeInitialState(
    const MatchSetting& setting,
    const std::vector<AgentKind>& kinds) {
    if (kinds.size() != setting.agentCount()) {
        throw std::invalid_argument(
            "agent kind count must match agent starts");
    }

    MatchState state;
    state.day = 0;
    state.trafficByPos.assign(
        setting.field.cellCount(), TrafficStatus::Smooth);
    state.agents.reserve(setting.agentCount());
    for (std::size_t i = 0; i < setting.agentCount(); ++i) {
        state.agents.push_back(Agent{
            i,
            kinds[i],
            setting.agentStarts[i],
            setting.fuelLimit,
        });
    }
    return state;
}

inline MatchState makeInitialState(
    const MatchSetting& setting,
    std::int32_t supplyCount) {
    if (supplyCount < 0 ||
        supplyCount >= static_cast<std::int32_t>(setting.agentCount())) {
        throw std::invalid_argument(
            "supplies must be in [0, agentCount - 1]");
    }

    std::vector<AgentKind> kinds(
        setting.agentCount(), AgentKind::Patrol);
    const std::size_t firstSupply =
        setting.agentCount() - static_cast<std::size_t>(supplyCount);
    for (std::size_t i = 0; i < setting.agentCount(); ++i) {
        if (i >= firstSupply) {
            kinds[i] = AgentKind::Supply;
        }
    }
    return makeInitialState(setting, kinds);
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
        Brain1::search(setting, state, previousScore, config);
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
    output << "[\n";
    for (std::size_t agent = 0; agent < plan.size(); ++agent) {
        output << "  [";
        for (std::size_t action = 0; action < plan[agent].size(); ++action) {
            if (action != 0) {
                output << ", ";
            }
            output << plan[agent][action];
        }
        output << "]" << (agent + 1 == plan.size() ? "\n" : ",\n");
    }
    output << "]\n";
}

inline int create(
    int argc,
    char* argv[],
    const ApplicationConfig& defaults = {}) {
    const ApplicationConfig options = application_detail::parseOptions(
        argc, argv, true, defaults);
    const GeneratedProblem problem =
        ProblemCreator::generate(options.createConfig);
    if (!ProblemCreator::writeJson(problem, options.outputPath)) {
        std::cerr << "failed to write: " << options.outputPath << '\n';
        return 1;
    }

    std::cout << "created " << options.outputPath
              << " (seed=" << problem.seed
              << ", map=" << problem.setting.field.width << 'x'
              << problem.setting.field.height
              << ", agents=" << problem.setting.agentCount()
              << ", spots=" << problem.setting.field.spots.size()
              << ", brands=" << problem.setting.field.brands.size()
              << ")\n";
    return 0;
}

inline int runLocalSolver(
    int argc,
    char* argv[],
    const ApplicationConfig& defaults = {}) {
    const ApplicationConfig options = application_detail::parseOptions(
        argc, argv, false, defaults);
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

}  // namespace procon37
