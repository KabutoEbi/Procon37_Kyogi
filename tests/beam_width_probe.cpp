#include "application.hpp"
#include "solver_options.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cerr << "usage: beam_width_probe setting state-or-- score-or-- width-or-auto\n";
            return 2;
        }
        const std::string_view widthText(argv[4]);
        std::size_t width = 0;
        if (widthText != "auto") {
            const auto parsed = std::from_chars(widthText.data(),
                widthText.data() + widthText.size(), width);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != widthText.data() + widthText.size() ||
                width == 0 || width > 100000) {
                throw std::invalid_argument("invalid beam width");
            }
        }

        const auto setting = benzen::readMatchSettingJson(argv[1]);
        auto state = std::string_view(argv[2]) == "-"
            ? benzen::makeInitialState(setting, 1)
            : benzen::readMatchStateJson(argv[2], setting);
        // Recorded match deadlines have expired; this probe measures natural
        // completion rather than stopping at the original server deadline.
        state.endsAt = 0;
        benzen::ScoreState score;
        if (std::string_view(argv[3]) != "-") {
            const auto checkpoint = benzen::readScoreCheckpointJson(argv[3]);
            if (checkpoint.nextDay != state.day)
                throw std::invalid_argument("score checkpoint day mismatch");
            score = checkpoint.score;
        } else if (state.day != 0) {
            throw std::invalid_argument("score checkpoint required after day 0");
        }

        benzen::BeamSearchConfig config;
        config.beamWidth = widthText == "auto" ? 128 : width;
        config.jointBeamWidth = config.beamWidth * 4;
        config.timeLimitMs = 0;
        config.useStateDeadline = false;
        config.adaptiveBeamWidth = false;
        config.iterativeDeepening = widthText == "auto";
        config.comparePeriodicReplanPlans = widthText == "auto";
        if (widthText == "auto") {
            config.timeLimitMs = 50000;
            config = benzen::SolverOptions{}.beamFor(setting, state, config);
        }

        const auto started = std::chrono::steady_clock::now();
        const auto result = benzen::brain1(setting, state, score, config);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "day=" << state.day << " width=" << config.beamWidth
                  << " joint=" << config.jointBeamWidth
                  << " elapsed_ms=" << elapsed
                  << " success=" << result.success
                  << " stop=" << static_cast<int>(result.stopReason)
                  << " match_brands=" <<
                        benzen::countBits(result.finalState.score.matchBrands)
                  << " day_brands=" << benzen::countBits(result.finalState.dayBrands)
                  << " total_udon=" << result.finalState.score.totalUdon
                  << " expanded=" << result.expandedNodes
                  << " spot_attempts=" << result.spotRouteLnsAttempts
                  << " alns_attempts=" << result.udonAlnsAttempts << '\n';
        return result.success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
