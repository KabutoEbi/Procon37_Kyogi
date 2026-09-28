#include "inc/application.hpp"
#include "inc/solver_options.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace benzen;

int runSelection(int argc, char* argv[], const SolverOptions& options) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: " << argv[0]
                  << " select match-setting.json [agents.json]\n";
        return 1;
    }
    const auto setting = readMatchSettingJson(argv[2]);
    const auto selected = selectAgentKinds(setting, options.selectorFor(setting));
    if (!selected.success) {
        std::cerr << "failed to select agent kinds\n";
        return 1;
    }
    const std::string output = argc == 4 ? argv[3] : "agents.json";
    if (!writeAgentKindsJson(selected.kinds, output)) {
        std::cerr << "failed to write " << output << '\n';
        return 1;
    }
    std::cout << "agents-json=" << output
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

}  // namespace

int main(int argc, char* argv[]) {
    try {
        auto options = benzen::SolverOptions::parse(argc, argv);
        argc = static_cast<int>(options.positional.size());
        argv = options.positional.data();
        const bool selectMode = argc >= 2 && std::string_view(argv[1]) == "select";
        const bool solveMode = argc >= 2 && std::string_view(argv[1]) == "solve";
        if (selectMode) return runSelection(argc, argv, options);

        const int firstArg = solveMode ? 2 : 1;
        if ((solveMode && (argc < 4 || argc > 5)) ||
            (!solveMode && argc > 4)) {
            std::cerr << "usage: " << argv[0]
                      << " [match-setting.json [match-state.json"
                         " [score-checkpoint.json]]]\n"
                      << "       " << argv[0]
                      << " select match-setting.json [agents.json]\n"
                      << "       " << argv[0]
                      << " solve match-setting.json match-state.json"
                         " [score-checkpoint.json]\n";
            return 1;
        }

        benzen::MatchSetting setting;
        if (argc > firstArg) {
            setting = benzen::readMatchSettingJson(argv[firstArg]);
        } else {
            setting = benzen::create(
                benzen::CreateConfig{}, "problem.json").setting;
        }

        benzen::MatchState st;
        std::vector<benzen::AgentKind> kinds;
        if (argc > firstArg + 1) {
            st = benzen::readMatchStateJson(argv[firstArg + 1], setting);
            kinds.reserve(st.agents.size());
            for (const auto& agent : st.agents) kinds.push_back(agent.kind);
        } else {
            const auto sel = benzen::selectAgentKinds(
                setting, options.selectorFor(setting));
            if (!sel.success) {
                std::cerr << "failed to select agent kinds\n";
                return 1;
            }
            kinds = sel.kinds;
            st = benzen::makeInitialState(setting, kinds);
            if (sel.timedOut) {
                std::cerr << "agent selection stopped at deadline\n";
            }
        }

        if (!benzen::writeAgentKindsJson(kinds, "agents.json")) {
            std::cerr << "failed to write agents.json\n";
            return 1;
        }

        benzen::ScoreState previousScore;
        const char* scorePath = argc > firstArg + 2 ? argv[firstArg + 2] : nullptr;
        if (!loadScore(scorePath, setting, st, previousScore)) return 1;

        auto beam = options.beam;
        if (st.endsAt == 0 && !options.hasTimeLimit) {
            // sample_maps等のオフライン実行も無制限にしない。
            beam.timeLimitMs = std::min<std::int64_t>(
                5000,
                static_cast<std::int64_t>(setting.daySeconds[
                    static_cast<std::size_t>(st.day)]) * 1000);
        }

        std::cout << "parameters beam-width=" << beam.beamWidth
                  << " joint-beam-width=" << beam.jointBeamWidth
                  << " threads=" << beam.threadCount
                  << " time-limit-ms=" << beam.timeLimitMs
                  << " safety-margin-ms=" << beam.safetyMarginMs << '\n';

        const auto ans = benzen::solveDay(
            setting, st, previousScore, beam);
        if (!ans.success) {
            std::cerr << "failed to construct even a wait plan\n";
            return 1;
        }

        const auto validation = benzen::validateDailyPlan(
            setting, st, ans.plan, previousScore);
        if (!validation.valid) {
            std::cerr << "refusing to write invalid plan: "
                      << benzen::simulationErrorMessage(
                             validation.failure.error)
                      << '\n';
            return 1;
        }
        constexpr const char* answerPath = "answer.json";
        if (!benzen::writePlanJson(ans.plan, answerPath)) {
            std::cerr << "failed to write " << answerPath << '\n';
            return 1;
        }
        const benzen::ScoreCheckpoint nextCheckpoint{
            st.day + 1,
            ans.finalState.score,
        };
        if (!benzen::writeScoreCheckpointJson(
                nextCheckpoint, "score-next.json")) {
            std::cerr << "failed to write score-next.json\n";
            return 1;
        }
        constexpr const char* resultStatePath = "result-state.json";
        if (!benzen::writeResultStateJson(ans.finalState, resultStatePath)) {
            std::cerr << "failed to write " << resultStatePath << '\n';
            return 1;
        }

        benzen::ConsoleConfig console;
        console.showFinalMap = false;
        console.showPlan = false;
        console.showTrace = false;
        benzen::printConsoleReport(
            std::cout, setting, st, ans, previousScore, console);
        std::cout << "\nanswer-json=" << answerPath
                  << "\nagents-json=agents.json"
                  << "\nnext-score-json=score-next.json"
                  << "\nresult-state-json=" << resultStatePath << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
