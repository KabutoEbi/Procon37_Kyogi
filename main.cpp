#include "inc/application.hpp"

#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    try {
        if (argc > 2) {
            std::cerr << "usage: " << argv[0] << " [match-setting.json]\n";
            return 1;
        }

        procon37::MatchSetting setting;
        if (argc == 2) {
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

        procon37::AgentSelectorConfig selCfg;
        selCfg.trialCandidateLimit = 8;
        selCfg.trialBeamWidth = 32;
        selCfg.trialJointBeamWidth = 128;
        const auto sel = procon37::selectAgentKinds(setting, selCfg);
        if (!sel.success) {
            std::cerr << "failed to select agent kinds\n";
            return 1;
        }
        const auto st = procon37::makeInitialState(setting, sel.kinds);

        [[maybe_unused]] const auto path = procon37::brain0(
            setting,
            st,
            st.agents.front().pos,
            setting.field.spots.front().pos,
            st.agents.front().kind);

        procon37::BeamSearchConfig beam;
        beam.beamWidth = 128;
        beam.jointBeamWidth = 512;

        const auto ans = procon37::brain1(setting, st, {}, beam);
        procon37::ConsoleConfig console;
        console.showFinalMap = false;
        console.showPlan = false;
        console.showTrace = false;
        procon37::printConsoleReport(
            std::cout, setting, st, ans, {}, console);
        if (!ans.success) {
            return 1;
        }

        const auto validation = procon37::validateDailyPlan(
            setting, st, ans.plan);
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
        std::cout << "\nanswer-json=" << answerPath << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
