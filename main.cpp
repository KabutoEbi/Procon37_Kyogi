#include "inc/application.hpp"
#include "inc/solver_options.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace benzen;

constexpr const char* kAgentsPath = "agents.json";
constexpr const char* kAnswerPath = "answer.json";
constexpr const char* kScorePath = "score-next.json";
constexpr const char* kStatePath = "result-state.json";

AgentSelectionResult selectKinds(const MatchSetting& setting,
                                 const SolverOptions& options) {
    auto result = selectAgentKinds(setting, options.selectorFor(setting));
    if (!result.success) std::cerr << "failed to select agent kinds\n";
    return result;
}

bool reportWrite(bool success, const char* path) {
    if (!success) std::cerr << "failed to write " << path << '\n';
    return success;
}

void printMainUsage(const char* program) {
    std::cerr << "usage: " << program
              << " [match-setting.json [match-state.json"
                 " [score-checkpoint.json]]]\n"
              << "       " << program
              << " select match-setting.json [agents.json]\n"
              << "       " << program
              << " solve match-setting.json match-state.json"
                 " [score-checkpoint.json]\n"
              << "       " << program
              << " generate [random-setting.json] [options]\n";
}

int runSelection(int argc, char* argv[], const SolverOptions& options) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: " << argv[0]
                  << " select match-setting.json [agents.json]\n";
        return 1;
    }
    const auto setting = readMatchSettingJson(argv[2]);
    const auto selected = selectKinds(setting, options);
    if (!selected.success) return 1;
    const char* output = argc == 4 ? argv[3] : kAgentsPath;
    if (!reportWrite(writeAgentKindsJson(selected.kinds, output), output)) {
        return 1;
    }
    std::cout << "agents-json=" << output
              << "\nselected-patrols=" << setting.agentCount() - selected.supplyCount
              << "\nselected-supplies=" << selected.supplyCount
              << "\npredicted-top-udon=" << predictArenaTopScore(setting).targetUdon
              << "\npredicted-minimum-patrols="
              << predictArenaTopScore(setting).minimumPatrols
              << "\ntimed-out=" << (selected.timedOut ? "true" : "false") << '\n';
    return 0;
}

bool loadScore(const char* path, const MatchSetting& setting,
               const MatchState& state, ScoreState& score) {
    if (!path) {
        if (state.day == 0) return true;
        std::cerr << "a score checkpoint is required after day 0\n";
        return false;
    }
    const auto checkpoint = readScoreCheckpointJson(path);
    if (checkpoint.nextDay != state.day) {
        std::cerr << "score checkpoint day does not match state day\n";
        return false;
    }
    score = checkpoint.score;
    const auto brandCount = setting.field.brands.size();
    if (brandCount < kMaxBrands && (score.matchBrands >> brandCount) != 0) {
        std::cerr << "score checkpoint contains an unknown brand\n";
        return false;
    }
    return true;
}

int runSolver(int argc, char* argv[], int firstArg, const SolverOptions& options) {
    const auto setting = argc > firstArg
        ? readMatchSettingJson(argv[firstArg])
        : create(CreateConfig{}, "problem.json").setting;

    MatchState state;
    std::vector<AgentKind> kinds;
    if (argc > firstArg + 1) {
        state = readMatchStateJson(argv[firstArg + 1], setting);
        kinds.reserve(state.agents.size());
        for (const auto& agent : state.agents) kinds.push_back(agent.kind);
    } else {
        const auto selected = selectKinds(setting, options);
        if (!selected.success) return 1;
        kinds = selected.kinds;
        state = makeInitialState(setting, kinds);
        if (selected.timedOut) {
            std::cerr << "agent selection stopped at deadline\n";
        }
    }

    ScoreState score;
    const char* checkpointPath = argc > firstArg + 2 ? argv[firstArg + 2] : nullptr;
    if (!loadScore(checkpointPath, setting, state, score)) return 1;

    auto config = options.beam;
    if (state.endsAt == 0 && !options.hasTimeLimit && !config.exactBrands) {
        // オフライン実行も無制限にしない。60秒枠の5秒前を目標にする。
        config.timeLimitMs = std::min<std::int64_t>(
            options.autoBeamWidth ? 60000 : 5000 + config.safetyMarginMs,
            static_cast<std::int64_t>(setting.daySeconds[
                static_cast<std::size_t>(state.day)]) * 1000);
    }
    config = options.beamFor(setting, state, config);
    if (config.timeLimitMs > 0 && config.timeLimitMs <= config.safetyMarginMs) {
        std::cerr << "time limit must exceed safety margin\n";
        return 1;
    }
    if (!reportWrite(writeAgentKindsJson(kinds, kAgentsPath), kAgentsPath)) return 1;

    std::cout << "parameters brain=2"
              << " beam-width=" << config.beamWidth
              << " joint-beam-width=" << config.jointBeamWidth
              << " threads=" << config.threadCount
              << " time-limit-ms=" << config.timeLimitMs
              << " safety-margin-ms=" << config.safetyMarginMs
              << " auto-beam-width=" << (config.autoSizedBeam ? 1 : 0)
              << " route-columns=" << (config.routeColumns.enabled ? 1 : 0)
              << " terminal-wait-fill="
              << (config.spotRouteLns.terminalWaitFill ? 1 : 0)
              << " traffic-aware-joint-routes="
              << (config.spotRouteLns.trafficAwareJointRoutes ? 1 : 0)
              << " small-board-quantity-first="
              << (config.smallBoardQuantityFirstRoutes ? 1 : 0)
              << " quantity-budget-percent=" << config.quantityBudgetPercent
              << " adaptive-long-search=" << (config.adaptiveLongSearch ? 1 : 0)
              << " udon-sa=" << (config.udonAlns.simulatedAnnealing ? 1 : 0)
              << " stable-beam-stop=" << (config.stopOnStableBeam ? 1 : 0)
              << " sa-stagnation-stop="
              << (config.udonAlns.stopOnStagnation ? 1 : 0)
              << " small-board-beam-budget-percent="
              << config.smallBoardBeamBudgetPercent
              << " small-board-brand-node-limit="
              << config.smallBoardBrandSearchNodeLimit
              << " day-beam=" << (config.useDayBeam ? 1 : 0)
              << " day-beam-width=" << config.dayBeamWidth
              << " day-beam-depth=" << config.dayBeamDepth
              << " day-beam-max-udon-loss=" << config.dayBeamMaxUdonLoss
              << '\n';

    const auto result = solveDay(setting, state, score, config);
    if (!result.success) {
        std::cerr << "failed to construct even a wait plan\n";
        return 1;
    }

    const auto validation = validateDailyPlan(setting, state, result.plan, score);
    if (!validation.valid) {
        std::cerr << "refusing to write invalid plan: "
                  << simulationErrorMessage(validation.failure.error)
                  << '\n';
        return 1;
    }
    if (!reportWrite(writePlanJson(result.plan, kAnswerPath), kAnswerPath)) {
        return 1;
    }
    const ScoreCheckpoint checkpoint{
        state.day + 1,
        result.finalState.score,
    };
    if (!reportWrite(
            writeScoreCheckpointJson(checkpoint, kScorePath), kScorePath)) {
        return 1;
    }
    if (!reportWrite(
            writeResultStateJson(result.finalState, kStatePath), kStatePath)) {
        return 1;
    }

    ConsoleConfig consoleConfig;
    consoleConfig.showFinalMap = false;
    consoleConfig.showPlan = false;
    consoleConfig.showTrace = false;
    printConsoleReport(std::cout, setting, state, result, score, consoleConfig);
    std::cout << "\nanswer-json=" << kAnswerPath
              << "\nagents-json=" << kAgentsPath
              << "\nnext-score-json=" << kScorePath
              << "\nresult-state-json=" << kStatePath << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "generate") {
            benzen::ApplicationConfig defaults;
            defaults.outputPath = "random-setting.json";
            return benzen::createMatchSetting(argc - 2, argv + 2, defaults);
        }
        auto options = benzen::SolverOptions::parse(argc, argv);
        argc = static_cast<int>(options.positional.size());
        argv = options.positional.data();
        const std::string_view command = argc >= 2 ? argv[1] : "";
        if (command == "select") return runSelection(argc, argv, options);
        const bool solveMode = command == "solve";

        const int firstArg = solveMode ? 2 : 1;
        if ((solveMode && (argc < 4 || argc > 5)) ||
            (!solveMode && argc > 4)) {
            printMainUsage(argv[0]);
            return 1;
        }

        return runSolver(argc, argv, firstArg, options);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
