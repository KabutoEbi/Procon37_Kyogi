#pragma once

#include "brain/brain1.hpp"
#include "simulator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace procon37 {

struct ConsoleConfig {
    bool showMap = true;
    bool showFinalMap = false;
    bool showPlan = true;
    bool showEvaluation = true;
    bool showTrace = true;
    bool traceEveryStep = false;
    std::size_t maxTraceLines = 120;
};

inline const char* agentKindName(AgentKind kind) noexcept {
    return kind == AgentKind::Patrol ? "patrol" : "supply";
}

inline const char* searchStopReasonName(SearchStopReason reason) noexcept {
    switch (reason) {
        case SearchStopReason::Completed:
            return "completed";
        case SearchStopReason::TimeLimit:
            return "time-limit";
        case SearchStopReason::NoValidPlan:
            return "no-valid-plan";
    }
    return "unknown";
}

inline const char* directionName(std::int32_t action) noexcept {
    switch (action) {
        case 0:
            return "upper-left";
        case 1:
            return "upper-right";
        case 2:
            return "right";
        case 3:
            return "lower-right";
        case 4:
            return "lower-left";
        case 5:
            return "left";
        default:
            return "wait";
    }
}

inline std::string consoleCellToken(
    const Field& field,
    CellPos pos,
    const MatchState* state = nullptr) {
    std::int32_t value = static_cast<std::int32_t>(field.at(pos).type);
    if (field.at(pos).type == CellType::Road && state != nullptr) {
        switch (state->trafficAt(pos)) {
            case TrafficStatus::Smooth:
                value = 1;
                break;
            case TrafficStatus::Busy:
                value = 4;
                break;
            case TrafficStatus::Jammed:
                value = 5;
                break;
        }
    }

    std::ostringstream token;
    if (field.at(pos).hasSpot()) {
        const auto spotIndex =
            static_cast<std::size_t>(field.at(pos).spotIndex);
        token << '[' << field.spots[spotIndex].brand << ']';
    } else {
        token << value;
    }
    return token.str();
}

inline void printConsoleMap(
    std::ostream& out,
    const MatchSetting& setting,
    const MatchState* state = nullptr) {
    const Field& field = setting.field;
    for (std::int32_t y = 0; y < field.height; ++y) {
        // この座標系では偶数行が右へずれる。
        out << ((y & 1) == 0 ? "  " : "");
        for (std::int32_t x = 0; x < field.width; ++x) {
            const CellPos pos = field.toPos({x, y});
            out << consoleCellToken(field, pos, state) << ' ';
        }
        out << '\n';
    }
}

inline void printConsoleAgents(
    std::ostream& out,
    const MatchSetting& setting,
    const std::vector<Agent>& agents) {
    out << "agents\n";
    for (const Agent& agent : agents) {
        const Coord pos = setting.field.toCoord(agent.pos);
        out << '[' << agent.index << ']'
            << (agent.isPatrol() ? 'P' : 'S')
            << " [pos]" << agent.pos << '(' << pos.x << ',' << pos.y << ')'
            << " [fuel]" << agent.fuel << '/' << setting.fuelLimit << '\n';
    }
}

inline void printConsolePlan(
    std::ostream& out,
    const MatchSetting& setting,
    const MatchState& state,
    const DailyPlan& plan) {
    out << "plan\n";
    const DailyPlan compressed = compressWaitActions(plan);
    for (AgentIndex agent = 0; agent < compressed.size(); ++agent) {
        CellPos pos = state.agents[agent].pos;
        std::int32_t elapsed = 0;
        out << "  agent " << agent << ':';
        for (const std::int32_t action : compressed[agent]) {
            if (isWaitAction(action)) {
                elapsed += waitSteps(action);
                out << " wait(" << waitSteps(action) << ')';
                continue;
            }
            const std::int32_t steps = moveStepCost(setting, state, pos);
            const CellPos next = setting.field.neighbor(
                pos, static_cast<Direction>(action));
            elapsed += steps;
            out << ' ' << directionName(action) << '[' << pos << "->" << next
                << "," << steps << "step]";
            pos = next;
        }
        out << "  total=" << elapsed << '\n';
    }
}

inline void printConsoleEvaluation(
    std::ostream& out,
    const EvaluationBreakdown& evaluation) {
    out << "evaluation\n"
        << "  total=" << std::setprecision(12) << evaluation.total << '\n'
        << "  new-brands=" << evaluation.newBrandCount << '\n'
        << "  daily-brands=" << evaluation.dailyBrandCount << '\n'
        << "  udon=" << evaluation.udonCount << '\n'
        << "  fuel=" << evaluation.remainingFuel << '\n'
        << "  unreachable=" << evaluation.unreachableTargetCount << '\n'
        << "  fuel-risk=" << evaluation.fuelRisk << '\n'
        << "  next-day-udon=" << evaluation.nextDayUdonCount << '\n';
}

inline bool printConsoleTrace(
    std::ostream& out,
    const MatchSetting& setting,
    const MatchState& match,
    const DailyPlan& sourcePlan,
    ScoreState previousScore = {},
    const ConsoleConfig& config = {}) {
    const DailyPlan plan = compressWaitActions(sourcePlan);
    const SimulationResult checked = simulateDay(
        setting, match, plan, previousScore);
    if (!checked.valid) {
        out << "trace invalid: " << simulationErrorMessage(checked.failure.error)
            << " agent=" << checked.failure.agentIndex
            << " action=" << checked.failure.actionIndex
            << " step=" << checked.failure.step << '\n';
        return false;
    }

    GameState state;
    if (!state.initialize(setting, match, previousScore)) {
        out << "trace invalid: state initialization failed\n";
        return false;
    }
    std::array<std::size_t, kMaxAgents> cursors{};
    for (AgentIndex agent = 0; agent < state.agentCount; ++agent) {
        if (plan[agent].empty()) {
            out << "trace invalid: missing action agent=" << agent << '\n';
            return false;
        }
        const auto failure = tryScheduleAction(
            state, setting, match, agent, plan[agent][0], 0);
        if (failure.error != SimulationError::None) {
            return false;
        }
        ++cursors[agent];
    }

    out << "trace\n";
    std::size_t printed = 0;
    bool omitted = false;
    for (std::int32_t step = 1; step <= state.dayStepLimit; ++step) {
        const auto beforeAgents = state.agents;
        const std::int32_t beforeUdon = state.dayUdon;
        const BrandMask beforeBrands = state.dayBrands;
        const auto failure = advanceSimulationStep(state, setting);
        if (failure.error != SimulationError::None) {
            return false;
        }
        bool completed = false;
        for (AgentIndex agent = 0; agent < state.agentCount; ++agent) {
            completed = completed ||
                        (beforeAgents[agent].pos != state.agents[agent].pos);
            if (beforeAgents[agent].fuel != state.agents[agent].fuel) {
                completed = true;
            }
        }
        const bool changed = completed || beforeUdon != state.dayUdon ||
                             beforeBrands != state.dayBrands ||
                             step == state.dayStepLimit;
        if (config.traceEveryStep || changed) {
            if (printed < config.maxTraceLines) {
                out << "  step " << std::setw(3) << step
                    << " udon=" << state.dayUdon
                    << " brands=" << countBits(state.dayBrands);
                for (AgentIndex agent = 0; agent < state.agentCount; ++agent) {
                    out << " | " << (state.agents[agent].isPatrol() ? 'P' : 'S')
                        << agent << '@' << state.agents[agent].pos;
                    if (state.agents[agent].isPatrol()) {
                        out << " f=" << state.agents[agent].fuel;
                    }
                }
                out << '\n';
                ++printed;
            } else {
                omitted = true;
            }
        }

        if (step == state.dayStepLimit) {
            continue;
        }
        for (AgentIndex agent = 0; agent < state.agentCount; ++agent) {
            if (!state.pendingActions[agent].active) {
                if (cursors[agent] >= plan[agent].size()) {
                    out << "trace invalid: action exhausted agent=" << agent
                        << " step=" << step << '\n';
                    return false;
                }
                const auto failure = tryScheduleAction(
                    state,
                    setting,
                    match,
                    agent,
                    plan[agent][cursors[agent]],
                    cursors[agent]);
                if (failure.error != SimulationError::None) {
                    return false;
                }
                ++cursors[agent];
            }
        }
    }
    if (omitted) {
        out << "  ... trace truncated after " << config.maxTraceLines
            << " lines\n";
    }
    return true;
}

inline void printConsoleReport(
    std::ostream& out,
    const MatchSetting& setting,
    const MatchState& start,
    const BeamSearchResult& result,
    ScoreState previousScore = {},
    const ConsoleConfig& config = {}) {
    out << "=== procon37 solver report ===\n"
        << "day=" << start.day << '\n'
        << "steps=" << setting.daySteps[static_cast<std::size_t>(start.day)]
        << '\n'
        << "fuel-limit=" << setting.fuelLimit << '\n'
        << "spots=" << setting.field.spots.size() << '\n'
        << "brands=" << setting.field.brands.size() << "\n\n";
    if (config.showMap) {
        printConsoleMap(out, setting, &start);
        out << '\n';
    }
    printConsoleAgents(out, setting, start.agents);
    out << "\nsearch\n"
        << "  success=" << std::boolalpha << result.success << '\n'
        << "  stop=" << searchStopReasonName(result.stopReason) << '\n'
        << "  elapsed-ms=" << result.elapsedMs << '\n'
        << "  expanded=" << result.expandedNodes << '\n'
        << "  evaluations=" << result.fullEvaluations << '\n'
        << "  skipped-evaluations=" << result.reusedEvaluations << '\n'
        << "  macro-candidates=" << result.macroCandidates << '\n'
        << "  macro-continuations=" << result.macroContinuations << '\n';
    if (!result.success) {
        return;
    }
    if (config.showPlan) {
        printConsolePlan(out, setting, start, result.plan);
    }
    if (config.showTrace) {
        (void)printConsoleTrace(
            out, setting, start, result.plan, previousScore, config);
    }
    if (config.showEvaluation) {
        out << '\n';
        printConsoleEvaluation(out, result.evaluation);
    }
    std::vector<Agent> finalAgents(
        result.finalState.agents.begin(),
        result.finalState.agents.begin() + result.finalState.agentCount);
    out << "\nresult\n"
        << "  udon=" << result.finalState.dayUdon << '\n'
        << "  daily-brands=" << countBits(result.finalState.dayBrands) << '\n'
        << "  match-brands=" << countBits(result.finalState.score.matchBrands)
        << '\n'
        << "  total-udon=" << result.finalState.score.totalUdon << '\n';
    out << '\n';
    printConsoleAgents(out, setting, finalAgents);
    if (config.showFinalMap) {
        out << "final ";
        printConsoleMap(out, setting, &start);
    }
}

}  // namespace procon37
