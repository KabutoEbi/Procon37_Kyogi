#include "agent_selector.hpp"
#include "create.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    using namespace benzen;
    int sizeOnly = 0, agentOnly = 0, spotOnly = 0, brandOnly = 0;
    int seedStart = 1, seeds = 3;
    int minSupply = -1, maxSupply = -1;
    int trialCandidates = -1;
    int selectionMs = 5000;
    bool constrained = false, mixedTraffic = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--constrained") {
                constrained = true;
                continue;
            }
            if (option == "--mixed-traffic") {
                mixedTraffic = true;
                continue;
            }
            if (++i == argc) throw std::invalid_argument("missing option value");
            const std::string argument = argv[i];
            std::size_t consumed = 0;
            const int value = std::stoi(argument, &consumed);
            if (consumed != argument.size())
                throw std::invalid_argument("invalid option value: " + argument);
            if (option == "--size") sizeOnly = value;
            else if (option == "--agents") agentOnly = value;
            else if (option == "--spots") spotOnly = value;
            else if (option == "--brands") brandOnly = value;
            else if (option == "--seed-start") seedStart = value;
            else if (option == "--seeds") seeds = value;
            else if (option == "--selection-ms") selectionMs = value;
            else if (option == "--min-supply") minSupply = value;
            else if (option == "--max-supply") maxSupply = value;
            else if (option == "--trial-candidates") trialCandidates = value;
            else throw std::invalid_argument("unknown option: " + option);
        }
        if ((sizeOnly != 0 && sizeOnly != 16 && sizeOnly != 24 && sizeOnly != 32) ||
            seedStart <= 0 || seeds <= 0 || selectionMs < 0 ||
            (agentOnly != 0 &&
             (agentOnly < 2 || agentOnly > static_cast<int>(kMaxAgents))) ||
            spotOnly < 0 || brandOnly < 0 ||
            minSupply < -1 || maxSupply < -1 ||
            trialCandidates < -1 ||
            (minSupply >= 0 && maxSupply >= 0 && minSupply > maxSupply) ||
            seedStart > std::numeric_limits<int>::max() - seeds)
            throw std::invalid_argument("invalid benchmark parameter");

        std::cout << "size,seed,steps,fuel,selection_ms,trial_assignments,"
                     "trial_simulations,used_trial,kinds,brands,daily_brands,udon,"
                     "holdout_brands,holdout_daily_brands,holdout_udon,completed_scenarios,timed_out\n";
        for (const int size : {16, 24, 32}) {
            if (sizeOnly != 0 && size != sizeOnly) continue;
            for (int seed = seedStart; seed < seedStart + seeds; ++seed) {
                CreateConfig create;
                create.width = create.height = size;
                create.agentCount = agentOnly > 0
                    ? agentOnly : (size == 16 ? 4 : size == 24 ? 5 : 7);
                create.spotCount = spotOnly > 0 ? spotOnly : size;
                create.brandCount = brandOnly > 0
                    ? brandOnly : (size == 16 ? 12 : size == 24 ? 18 : 24);
                create.seed = static_cast<std::uint64_t>(seed);
                auto setting = ProblemCreator::generate(create).setting;
                if (constrained) {
                    std::fill(setting.daySteps.begin(), setting.daySteps.end(), size);
                    setting.fuelLimit = size;
                }

                AgentSelectorConfig select;
                if (minSupply >= 0) select.minSupplyCount = minSupply;
                if (maxSupply >= 0) select.maxSupplyCount = maxSupply;
                if (trialCandidates >= 0) {
                    select.trialCandidateLimit =
                        static_cast<std::size_t>(trialCandidates);
                }
                select.timeLimitMs = selectionMs;
                select.safetyMarginMs = 0;
                select.trialBeamWidth = 8;
                select.trialJointBeamWidth = 32;
                const auto started = std::chrono::steady_clock::now();
                const auto chosen = selectAgentKinds(setting, select);
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                if (!chosen.success) throw std::runtime_error("agent selection failed");

                BeamSearchConfig beam;
                beam.beamWidth = 8;
                beam.jointBeamWidth = 32;
                beam.threadCount = 1;
                const auto validation = runTrial(setting, chosen.kinds, {}, beam);
                if (!validation.success)
                    throw std::runtime_error("validation trial failed");
                const auto score = trialScore(validation.score);
                auto holdoutTraffic = makeTrialScenarios(setting, 4).back();
                if (mixedTraffic) {
                    holdoutTraffic.assign(setting.dayCount(),
                        std::vector<TrafficStatus>(
                            setting.field.cellCount(), TrafficStatus::Smooth));
                    for (std::size_t day = 1; day < holdoutTraffic.size(); ++day) {
                        for (std::size_t pos = 0;
                             pos < holdoutTraffic[day].size(); ++pos) {
                            if (setting.field.cells[pos].type != CellType::Road)
                                continue;
                            holdoutTraffic[day][pos] = static_cast<TrafficStatus>(
                                (pos + static_cast<std::size_t>(seed) + day) % 3);
                        }
                    }
                }
                const auto holdout = runTrial(
                    setting, chosen.kinds, holdoutTraffic, beam);
                if (!holdout.success)
                    throw std::runtime_error("holdout trial failed");
                const auto holdoutScore = trialScore(holdout.score);

                std::cout << size << ',' << seed << ',' << setting.daySteps.front()
                          << ',' << setting.fuelLimit << ',' << elapsed << ','
                          << chosen.trialAssignments << ',' << chosen.trialSimulations
                          << ',' << chosen.usedBeamTrial << ',';
                for (const auto kind : chosen.kinds)
                    std::cout << (kind == AgentKind::Supply ? 'S' : 'P');
                std::cout << ',' << score[0] << ',' << score[1] << ',' << score[2]
                          << ',' << holdoutScore[0] << ',' << holdoutScore[1]
                          << ',' << holdoutScore[2] << ',' << chosen.completedTrialScenarios
                          << ',' << chosen.timedOut << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
