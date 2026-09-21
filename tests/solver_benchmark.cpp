#include "application.hpp"

#include <iostream>

int main() {
    using namespace procon37;
    std::cout << "size,seed,macros,brands,udon,ms,nodes,valid\n";
    for (const int size : {8, 16, 24, 32}) {
        for (const int seed : {1, 2}) {
            CreateConfig create;
            create.width = create.height = size;
            create.agentCount = size <= 16 ? 4 : 8;
            create.spotCount = size;
            create.brandCount = size * 3 / 4;
            create.seed = seed;
            const auto problem = ProblemCreator::generate(create);
            std::vector<AgentKind> kinds(create.agentCount, AgentKind::Patrol);
            kinds.back() = AgentKind::Supply;
            const auto state = makeInitialState(problem.setting, kinds);
            for (const bool macro : {false, true}) {
                BeamSearchConfig beam;
                beam.useMacroActions = macro;
                beam.threadCount = 1;
                beam.timeLimitMs = 300;
                beam.safetyMarginMs = 0;
                beam.useStateDeadline = false;
                const auto result = brain1(problem.setting, state, {}, beam);
                const bool valid = result.success && validateDailyPlan(
                    problem.setting, state, result.plan).valid;
                std::cout << size << ',' << seed << ',' << macro << ','
                          << countBits(result.finalState.score.matchBrands) << ','
                          << result.finalState.dayUdon << ',' << result.elapsedMs << ','
                          << result.expandedNodes << ',' << valid << '\n';
                if (!valid) return 1;
            }
        }
    }
}
