#include "application.hpp"
#include "solver_options.hpp"

#include <iostream>

// Read-only replay of an archived day's starting state. Ignore its historical
// wall-clock deadline, but retain traffic, vehicles, fuel and cumulative score.
int main(int argc, char** argv) {
    using namespace benzen;
    try {
        if (argc < 4 || argc > 6) {
            std::cerr << "usage: saved_day_benchmark setting state checkpoint-or-dash [time-ms] [coordinated-0-or-1]\n";
            return 2;
        }
        const auto setting = readMatchSettingJson(argv[1]);
        auto state = readMatchStateJson(argv[2], setting);
        ScoreState score;
        if (std::string_view(argv[3]) != "-") {
            const auto saved = readScoreCheckpointJson(argv[3]);
            if (saved.nextDay != state.day) throw std::invalid_argument("checkpoint day mismatch");
            score = saved.score;
        } else if (state.day != 0) throw std::invalid_argument("checkpoint required after day 0");
        state.endsAt = 0;
        SolverOptions options;
        options.hasTimeLimit = true;
        options.beam.timeLimitMs = argc > 4
            ? application_detail::parseInteger<std::int64_t>(argv[4], "time-ms") : 50000;
        if (options.beam.timeLimitMs <= 0) throw std::invalid_argument("positive time limit required");
        if (argc > 5) {
            const auto mode = application_detail::parseInteger<int>(argv[5], "coordinated");
            if (mode != 0 && mode != 1) throw std::invalid_argument("expected 0 or 1");
            options.beam.coordinatedRefinement = mode != 0;
        }
        const auto config = options.beamFor(setting, state, options.beam);
        const auto result = solveDay(setting, state, score, config);
        if (!result.success) return 1;
        const auto replay = simulateDay(setting, state, result.plan, score);
        if (!replay.valid || !(replay.state == result.finalState)) return 1;
        ConsoleConfig console;
        console.showFinalMap = false;
        console.showPlan = false;
        console.showTrace = false;
        printConsoleReport(std::cout, setting, state, result, score, console);
        std::cout << "replay-valid=true\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
