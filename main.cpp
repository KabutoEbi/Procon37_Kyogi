#include "application.hpp"

#include <iostream>

int main() {
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

    const auto problem = procon37::create(cfg, "problem.json");

    procon37::AgentSelectorConfig selCfg;
    selCfg.trialCandidateLimit = 8;
    selCfg.trialBeamWidth = 32;
    selCfg.trialJointBeamWidth = 128;
    const auto sel = procon37::selectAgentKinds(problem.setting, selCfg);
    if (!sel.success) {
        return 1;
    }
    const auto st = procon37::makeInitialState(problem.setting, sel.kinds);

    [[maybe_unused]] const auto path = procon37::brain0(
        problem.setting,
        st,
        st.agents.front().pos,
        problem.setting.field.spots.front().pos,
        st.agents.front().kind);

    procon37::BeamSearchConfig beam;
    beam.beamWidth = 128;
    beam.jointBeamWidth = 512;

    const auto ans = procon37::brain1(problem.setting, st, {}, beam);
    if (!ans.success) {
        return 1;
    }
    procon37::printPlan(std::cout, ans.plan);
    return 0;
}
