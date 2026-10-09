#include "application.hpp"

#include <iostream>
#include <string>
#include <vector>

// Read-only full-match replay using archived traffic for every day. Agent
// positions after day 0 are produced by the candidate plans, not copied from
// the historical run, so changes can improve later starting positions.
int main(int argc, char** argv) {
    using namespace benzen;
    try {
        if (argc < 2 || argc > 3) {
            std::cerr << "usage: runtime_match_benchmark runtime-dir [small-board-quantity-first-0-or-1]\n";
            return 2;
        }
        const std::string directory = argv[1];
        auto setting = readMatchSettingJson(directory + "/setting.json");
        setting.startsAt = 0;
        const auto initial = readMatchStateJson(
            directory + "/state-day0.json", setting);
        std::vector<AgentKind> kinds;
        for (const auto& agent : initial.agents) kinds.push_back(agent.kind);

        MatchRunnerConfig config;
        config.beam.timeLimitMs = 0;
        config.beam.safetyMarginMs = 0;
        config.beam.useStateDeadline = false;
        config.beam.adaptiveBeamWidth = false;
        config.beam.autoSizedBeam = false;
        if (argc == 3) {
            const int enabled = std::stoi(argv[2]);
            if (enabled != 0 && enabled != 1)
                throw std::invalid_argument("expected 0 or 1");
            config.beam.smallBoardQuantityFirstRoutes = enabled != 0;
        }
        for (std::size_t day = 0; day < setting.dayCount(); ++day) {
            const auto state = readMatchStateJson(directory + "/state-day" +
                std::to_string(day) + ".json", setting);
            if (state.day != static_cast<std::int32_t>(day))
                throw std::invalid_argument("archived state day mismatch");
            config.trafficByDay.push_back(state.trafficByPos);
        }

        const auto result = runMatch(setting, kinds, config);
        if (!result.success) {
            std::cerr << "failed on day " << result.failedDay << '\n';
            return 1;
        }
        for (const auto& day : result.days) {
            std::cout << day.day << ','
                      << countBits(day.endState.score.matchBrands) << ','
                      << day.endState.score.sumOfDailyBrandCounts << ','
                      << day.endState.score.totalUdon << ','
                      << day.elapsedMs << ','
                      << day.spotPortfolioMaxBrands << ','
                      << day.spotPortfolioMaxUdon << ','
                      << day.spotPortfolioMaxValidUdon << '\n';
        }
        std::cout << "result=" << countBits(result.score.matchBrands) << '/'
                  << result.score.sumOfDailyBrandCounts << '/'
                  << result.score.totalUdon << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
