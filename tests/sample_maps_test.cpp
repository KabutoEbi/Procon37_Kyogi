#include "application.hpp"

#include <array>
#include <cassert>
#include <iostream>
#include <string>

using namespace benzen;

int main() {
    constexpr std::array<const char*, 3> paths = {
        "sample_maps/sample_16.json",
        "sample_maps/sample_24.json",
        "sample_maps/sample_32.json",
    };
    constexpr std::array<std::int32_t, 3> expectedSupplyCounts = {1, 2, 2};

    for (std::size_t index = 0; index < paths.size(); ++index) {
        const char* path = paths[index];
        const MatchSetting setting = readMatchSettingJson(path);
        AgentSelectorConfig selector;
        selector.trialCandidateLimit = 0;
        selector.timeLimitMs = 500;
        selector.safetyMarginMs = 25;
        const AgentSelectionResult selected =
            selectAgentKinds(setting, selector);
        assert(selected.success);
        assert(selected.supplyCount == expectedSupplyCounts[index]);
        assert(std::count(
                   selected.kinds.begin(),
                   selected.kinds.end(),
                   AgentKind::Supply) >= 1);

        const MatchState state = makeInitialState(setting, selected.kinds);
        BeamSearchConfig beam;
        beam.beamWidth = 64;
        beam.jointBeamWidth = 256;
        beam.timeLimitMs = 750;
        beam.safetyMarginMs = 25;
        const BeamSearchResult answer =
            solveDay(setting, state, {}, beam);
        assert(answer.success);
        assert(validateDailyPlan(setting, state, answer.plan).valid);
        std::cout << path
                  << " agents=" << setting.agentCount()
                  << " udon=" << answer.finalState.dayUdon
                  << " elapsed-ms=" << answer.elapsedMs
                  << '\n';
    }
}
