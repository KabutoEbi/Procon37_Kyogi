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
    const auto state = procon37::makeInitialState(problem.setting, 1);

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
