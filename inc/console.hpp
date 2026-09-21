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
    const std::vector<Agent>* agents = nullptr) {
    std::size_t agentCount = 0;
    const Agent* only = nullptr;
    if (agents != nullptr) {
        for (const Agent& agent : *agents) {
            if (agent.pos == pos) {
                ++agentCount;
                only = &agent;
            }
        }
    }
    std::ostringstream token;
    if (agentCount > 1) {
        token << '*' << agentCount;
    } else if (only != nullptr) {
        token << (only->isPatrol() ? 'P' : 'S') << only->index;
    } else if (field.at(pos).hasSpot()) {
        token << 'U' << field.at(pos).spotIndex;
    } else {
        switch (field.at(pos).type) {
            case CellType::Plain:
                token << "..";
                break;
            case CellType::Road:
                token << "==";
                break;
            case CellType::Mountain:
                token << "^^";
                break;
            case CellType::Pond:
                token << "##";
                break;
        }
    }
    return token.str();
}

inline void printConsoleMap(
    std::ostream& out,
    const MatchSetting& setting,
    const std::vector<Agent>* agents = nullptr) {
    const Field& field = setting.field;
    out << "map " << field.width << 'x' << field.height
        << "  (P=patrol S=supply U=spot ==road ^^mountain ##pond)\n";
    for (std::int32_t y = 0; y < field.height; ++y) {
        // この座標系では偶数行が右へずれる。
        out << ((y & 1) == 0 ? "  " : "");
        for (std::int32_t x = 0; x < field.width; ++x) {
            const CellPos pos = field.toPos({x, y});
            out << std::setw(3) << consoleCellToken(field, pos, agents) << ' ';
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
        out << "  " << agent.index << ": " << agentKindName(agent.kind)
            << " pos=" << agent.pos << "(" << pos.x << ',' << pos.y << ')';
        if (agent.isPatrol()) {
            out << " fuel=" << agent.fuel << '/' << setting.fuelLimit;
        }
        out << '\n';
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
    out << "evaluation"
        << " total=" << std::setprecision(12) << evaluation.total
        << " new-brands=" << evaluation.newBrandCount
        << " daily-brands=" << evaluation.dailyBrandCount
        << " udon=" << evaluation.udonCount
        << " fuel=" << evaluation.remainingFuel
        << " unreachable=" << evaluation.unreachableTargetCount
        << " fuel-risk=" << evaluation.fuelRisk
        << " next-day-udon=" << evaluation.nextDayUdonCount << '\n';
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
        << "day=" << start.day
        << " steps=" << setting.daySteps[static_cast<std::size_t>(start.day)]
        << " fuel-limit=" << setting.fuelLimit
        << " spots=" << setting.field.spots.size()
        << " brands=" << setting.field.brands.size() << '\n';
    if (config.showMap) {
        printConsoleMap(out, setting, &start.agents);
    }
    printConsoleAgents(out, setting, start.agents);
    out << "search success=" << std::boolalpha << result.success
        << " stop=" << searchStopReasonName(result.stopReason)
        << " elapsed-ms=" << result.elapsedMs
        << " expanded=" << result.expandedNodes
        << " evaluations=" << result.fullEvaluations
        << " skipped-evaluations=" << result.reusedEvaluations
        << " macro-candidates=" << result.macroCandidates
        << " macro-continuations=" << result.macroContinuations << '\n';
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
        printConsoleEvaluation(out, result.evaluation);
    }
    std::vector<Agent> finalAgents(
        result.finalState.agents.begin(),
        result.finalState.agents.begin() + result.finalState.agentCount);
    out << "result udon=" << result.finalState.dayUdon
        << " daily-brands=" << countBits(result.finalState.dayBrands)
        << " match-brands=" << countBits(result.finalState.score.matchBrands)
        << " total-udon=" << result.finalState.score.totalUdon << '\n';
    printConsoleAgents(out, setting, finalAgents);
    if (config.showMap) {
        out << "final ";
        printConsoleMap(out, setting, &finalAgents);
    }
}

}  // namespace procon37
