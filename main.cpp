#include "application.hpp"

#include <iostream>

int main() {
    procon37::CreateConfig createArgs;
    createArgs.width = 16;
    createArgs.height = 16;
    createArgs.agentCount = 4;
    createArgs.spotCount = 16;
    createArgs.brandCount = 12;
    createArgs.dayCount = 4;
    createArgs.daySeconds = 60;
    createArgs.teamCount = 2;
    createArgs.seed = 1;

    const auto problem = procon37::create(createArgs, "problem.json");

    procon37::AgentSelectorConfig selectorArgs;
    selectorArgs.trialCandidateLimit = 8;
    selectorArgs.trialBeamWidth = 32;
    selectorArgs.trialJointBeamWidth = 128;
    const auto selection =
        procon37::selectAgentKinds(problem.setting, selectorArgs);
    if (!selection.success) {
        return 1;
    }
    const auto state =
        procon37::makeInitialState(problem.setting, selection.kinds);

    [[maybe_unused]] const auto shortestPath = procon37::brain0(
        problem.setting,
        state,
        state.agents.front().pos,
        problem.setting.field.spots.front().pos,
        state.agents.front().kind);

    procon37::BeamSearchConfig brain1Args;
    brain1Args.beamWidth = 128;
    brain1Args.jointBeamWidth = 512;

    const auto answer =
        procon37::brain1(problem.setting, state, {}, brain1Args);
    if (!answer.success) {
        return 1;
    }
    procon37::printPlan(std::cout, answer.plan);
    return 0;
}
