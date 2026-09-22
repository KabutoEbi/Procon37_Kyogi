#include "inc/application.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    try {
        if (argc > 4) {
            std::cerr << "usage: " << argv[0]
                      << " [match-setting.json [match-state.json"
                         " [score-checkpoint.json]]]\n";
            return 1;
        }

        procon37::MatchSetting setting;
        if (argc >= 2) {
            setting = procon37::readMatchSettingJson(argv[1]);
        } else {
            procon37::CreateConfig cfg;
            cfg.width = 16;
            cfg.height = 16;
            cfg.agentCount = 4;
            cfg.spotCount = 16;
            cfg.brandCount = 12;
            cfg.dayCount = 4;
            cfg.daySeconds = 60;
            cfg.teamCount = 2;
            cfg.seed = 1;
            setting = procon37::create(cfg, "problem.json").setting;
        }

        procon37::MatchState st;
        std::vector<procon37::AgentKind> kinds;
        if (argc >= 3) {
            st = procon37::readMatchStateJson(argv[2], setting);
            kinds.reserve(st.agents.size());
            for (const auto& agent : st.agents) kinds.push_back(agent.kind);
        } else {
            procon37::AgentSelectorConfig selCfg;
            selCfg.trialCandidateLimit = 8;
            selCfg.trialBeamWidth = 32;
            selCfg.trialJointBeamWidth = 128;
            const std::int32_t mapSize = std::max(
                setting.field.width, setting.field.height);
            selCfg.timeLimitMs = mapSize <= 16
                ? 60'000
                : (mapSize <= 24 ? 90'000 : 120'000);
            const auto sel = procon37::selectAgentKinds(setting, selCfg);
            if (!sel.success) {
                std::cerr << "failed to select agent kinds\n";
                return 1;
            }
            kinds = sel.kinds;
            st = procon37::makeInitialState(setting, kinds);
            if (sel.timedOut) {
                std::cerr << "agent selection stopped at deadline\n";
            }
        }

        if (!procon37::writeAgentKindsJson(kinds, "agents.json")) {
            std::cerr << "failed to write agents.json\n";
            return 1;
        }

        procon37::ScoreState previousScore;
        if (argc >= 4) {
            const auto checkpoint =
                procon37::readScoreCheckpointJson(argv[3]);
            if (checkpoint.nextDay != st.day) {
                std::cerr << "score checkpoint day does not match state day\n";
                return 1;
            }
            previousScore = checkpoint.score;
            if (setting.field.brands.size() < procon37::kMaxBrands &&
                (previousScore.matchBrands >>
                 setting.field.brands.size()) != 0) {
                std::cerr << "score checkpoint contains an unknown brand\n";
                return 1;
            }
        } else if (st.day != 0) {
            std::cerr << "a score checkpoint is required after day 0\n";
            return 1;
        }

        procon37::BeamSearchConfig beam;
        beam.beamWidth = 128;
        beam.jointBeamWidth = 512;
        if (st.endsAt == 0) {
            // sample_maps等のオフライン実行も無制限にしない。
            beam.timeLimitMs = std::min<std::int64_t>(
                5000,
                static_cast<std::int64_t>(setting.daySeconds[
                    static_cast<std::size_t>(st.day)]) * 1000);
        }

        const auto ans = procon37::solveDay(
            setting, st, previousScore, beam);
        if (!ans.success) {
            std::cerr << "failed to construct even a wait plan\n";
            return 1;
        }

        const auto validation = procon37::validateDailyPlan(
            setting, st, ans.plan, previousScore);
        if (!validation.valid) {
            std::cerr << "refusing to write invalid plan: "
                      << procon37::simulationErrorMessage(
                             validation.failure.error)
                      << '\n';
            return 1;
        }
        constexpr const char* answerPath = "answer.json";
        if (!procon37::writePlanJson(ans.plan, answerPath)) {
            std::cerr << "failed to write " << answerPath << '\n';
            return 1;
        }
        const procon37::ScoreCheckpoint nextCheckpoint{
            st.day + 1,
            ans.finalState.score,
        };
        if (!procon37::writeScoreCheckpointJson(
                nextCheckpoint, "score-next.json")) {
            std::cerr << "failed to write score-next.json\n";
            return 1;
        }

        procon37::ConsoleConfig console;
        console.showFinalMap = false;
        console.showPlan = false;
        console.showTrace = false;
        procon37::printConsoleReport(
            std::cout, setting, st, ans, previousScore, console);
        std::cout << "\nanswer-json=" << answerPath
                  << "\nagents-json=agents.json"
                  << "\nnext-score-json=score-next.json\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
