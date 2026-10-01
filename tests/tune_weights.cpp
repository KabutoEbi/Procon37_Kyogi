#include "create.hpp"
#include "weight_tuner.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    using namespace benzen;
    try {
        WeightTuningConfig config;
        config.candidates = 30;
        config.beam.beamWidth = 8;
        config.beam.jointBeamWidth = 24;
        config.beam.adaptiveBeamWidth = false;
        config.beam.fuelAwareWeights = false;
        int seeds = 4;
        EvaluationWeights initial;
        bool evaluateOnly = false, tightOnly = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--tight-only") { tightOnly = true; continue; }
            if (option == "--evaluate-only") { evaluateOnly = true; continue; }
            if (option == "--legacy-weights") {
                initial.remainingFuel = 1.0;
                initial.newBrandSpotDistance = -2000.0;
                initial.nextDayUdon = 2500.0;
                initial.nextDayBrandApproach = 1000.0;
                continue;
            }
            if (++i == argc) throw std::invalid_argument("missing option value");
            std::size_t consumed = 0;
            const int value = std::stoi(argv[i], &consumed);
            if (consumed != std::string(argv[i]).size() || value <= 0)
                throw std::invalid_argument("expected positive integer");
            if (option == "--time-ms") {
                config.fixedWork = false;
                config.beam.timeLimitMs = value;
            } else if (option == "--scale-percent") config.scale = value / 100.0;
            else if (option == "--candidates") config.candidates = value;
            else if (option == "--seeds") seeds = value;
            else if (option == "--beam") config.beam.beamWidth = value;
            else if (option == "--joint-beam") config.beam.jointBeamWidth = value;
            else throw std::invalid_argument("unknown option");
        }
        if (seeds > 1000) throw std::invalid_argument("at most 1000 seeds per split");
        std::vector<WeightTuningCase> training, validation;
        for (int split = 1; split <= 2; ++split) {
            for (const int size : {8, 16, 24, 32}) {
                for (int index = 0; index < seeds; ++index) {
                    if (tightOnly && index % 4 < 2) continue;
                    CreateConfig create;
                    create.width = create.height = size;
                    create.agentCount = size <= 16 ? 4 : 8;
                    create.spotCount = size;
                    create.brandCount = size * 3 / 4;
                    create.seed = static_cast<std::uint64_t>(split * 1000 + index + 1);
                    auto setting = ProblemCreator::generate(create).setting;
                    const bool mixed = index % 2 != 0;
                    const bool tight = index % 4 >= 2;
                    if (tight) setting.fuelLimit = std::max(1, setting.daySteps.front() / 2);
                    std::vector<AgentKind> kinds(create.agentCount, AgentKind::Patrol);
                    kinds.back() = AgentKind::Supply;
                    if (size > 16) kinds[kinds.size() - 2] = AgentKind::Supply;
                    TrialTraffic traffic(setting.dayCount(),
                        std::vector<TrafficStatus>(setting.field.cellCount()));
                    if (mixed)
                        for (std::size_t d = 0; d < traffic.size(); ++d)
                            for (std::size_t c = 0; c < traffic[d].size(); ++c)
                                traffic[d][c] = static_cast<TrafficStatus>((c + d + create.seed) % 3);
                    const auto id = create.seed * 100 + size;
                    WeightTuningCase sample{id, std::move(setting), std::move(kinds),
                        std::move(traffic), static_cast<std::uint64_t>(index % 4)};
                    (split == 1 ? training : validation).push_back(std::move(sample));
                }
            }
        }
        if (training.empty() || validation.empty())
            throw std::invalid_argument("no cases selected; --tight-only requires --seeds >= 3");
        WeightTuningResult result;
        if (evaluateOnly) {
            result.weights = EvaluationWeights{};
            result.trainingBefore = evaluateWeights(training, initial, config);
            auto candidateConfig = config;
            candidateConfig.beam.fuelAwareWeights = true;
            result.trainingAfter = evaluateWeights(training, result.weights, candidateConfig);
            const auto before = evaluateWeightGroups(validation, initial, config);
            const auto after = evaluateWeightGroups(validation, result.weights, candidateConfig);
            result.validationBefore = sumWeightScores(before);
            result.validationAfter = sumWeightScores(after);
            result.accepted = result.trainingAfter >= result.trainingBefore &&
                              weightGroupsNotWorse(before, after);
        } else {
            result = tuneWeights(training, validation, initial, config);
        }
        auto report = [](const char* name, const TrialScore& before, const TrialScore& after) {
            std::cerr << name << ": (" << before[0] << ',' << before[1] << ',' << before[2]
                      << ") -> (" << after[0] << ',' << after[1] << ',' << after[2] << ")\n";
        };
        report("training", result.trainingBefore, result.trainingAfter);
        report("validation", result.validationBefore, result.validationAfter);
        std::cerr << "cases=" << training.size() << '/' << validation.size()
                  << " accepted=" << result.accepted << '\n';
        printTunedWeights(std::cout, result.weights);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
