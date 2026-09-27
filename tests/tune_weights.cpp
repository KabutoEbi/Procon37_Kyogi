#include "create.hpp"
#include "weight_tuner.hpp"

#include <iostream>

int main() {
    using namespace benzen;
    std::vector<WeightTuningCase> training, validation;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        CreateConfig create;
        create.width = create.height = 8;
        create.agentCount = 3;
        create.spotCount = 8;
        create.brandCount = 5;
        create.seed = seed;
        auto problem = ProblemCreator::generate(create);
        auto scenarios = makeTrialScenarios(problem.setting);
        WeightTuningCase sample{seed, problem.setting,
            {AgentKind::Patrol, AgentKind::Patrol, AgentKind::Supply},
            scenarios[seed % scenarios.size()]};
        (seed <= 4 ? training : validation).push_back(std::move(sample));
    }
    WeightTuningConfig config;
    config.candidates = 6;
    config.trialDays = 2;
    config.beam.beamWidth = 8;
    config.beam.jointBeamWidth = 24;
    const auto result = tuneWeights(training, validation, {}, config);
    auto report = [](const char* name, const TrialScore& before, const TrialScore& after) {
        std::cerr << name << ": (" << before[0] << ',' << before[1] << ',' << before[2]
                  << ") -> (" << after[0] << ',' << after[1] << ',' << after[2] << ")\n";
    };
    report("training", result.trainingBefore, result.trainingAfter);
    report("validation", result.validationBefore, result.validationAfter);
    std::cerr << "accepted=" << result.accepted << '\n';
    printTunedWeights(std::cout, result.weights);
}
