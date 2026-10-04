#pragma once

#include "brain/brain1.hpp"
#include "simulator.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace benzen {

struct MatchRunnerConfig {
    BeamSearchConfig beam{};
    bool fallbackToWait = true;

    // 各日開始時にGET /で与えられる道路状態に相当する。
    // 空なら全日Smooth。指定する場合は日数×セル数と一致させる。
    std::vector<std::vector<TrafficStatus>> trafficByDay;
};

struct MatchDayResult {
    std::int32_t day = 0;
    bool usedFallback = false;
    MatchState startState{};
    DailyPlan plan;
    GameState endState{};
    EvaluationBreakdown evaluation{};
    std::vector<std::int32_t> roadStaySteps;
    std::size_t expandedNodes = 0;
    std::int64_t elapsedMs = 0;
    std::size_t assignmentCacheHits = 0;
    std::size_t assignmentComputations = 0;
    std::size_t removedSupplyLoops = 0;
    std::size_t brandRepairAttempts = 0, brandRepairExpanded = 0, brandRepairImprovements = 0;
    std::size_t udonAlnsAttempts = 0, udonAlnsExpanded = 0, udonAlnsImprovements = 0;
    std::size_t spotRouteLnsAttempts = 0, spotRouteLnsImprovements = 0;
    std::size_t routeColumnsGenerated = 0, routeColumnsMasterNodes = 0;
    std::size_t routeColumnsImprovements = 0;
    std::int64_t assignmentTimeNs = 0, flowTimeNs = 0, rendezvousTimeNs = 0;
    SearchStopReason stopReason = SearchStopReason::NoValidPlan;
};

struct MatchRunResult {
    bool success = false;
    std::vector<AgentKind> kinds;
    std::vector<MatchDayResult> days;
    ScoreState score{};
    std::int32_t failedDay = -1;
    SimulationFailure failure{};
};

class MatchRunner final {
private:
    static void validate(
        const MatchSetting& s,
        const std::vector<AgentKind>& kinds,
        const MatchRunnerConfig& cfg) {
        if (!s.hasConsistentDaySettings() || s.dayCount() == 0 ||
            kinds.size() != s.agentCount() ||
            s.agentCount() == 0 || s.agentCount() > kMaxAgents ||
            s.field.cells.size() != s.field.cellCount() ||
            s.fuelLimit <= 0 || s.playerCount <= 0) {
            throw std::invalid_argument("match setting is incomplete");
        }
        for (std::size_t day = 0; day < s.dayCount(); ++day) {
            if (s.daySteps[day] <= 0 || s.daySeconds[day] <= 0) {
                throw std::invalid_argument(
                    "day steps and seconds must be positive");
            }
        }
        for (const CellPos pos : s.agentStarts) {
            if (!s.field.contains(pos) || !s.field.at(pos).isPassable()) {
                throw std::invalid_argument(
                    "every agent start must be passable");
            }
        }
        for (const AgentKind kind : kinds) {
            if (kind != AgentKind::Patrol && kind != AgentKind::Supply) {
                throw std::invalid_argument("agent kind is invalid");
            }
        }
        if (s.busyThreshold <= 0 ||
            s.jammedThreshold <= s.busyThreshold) {
            throw std::invalid_argument("traffic thresholds are invalid");
        }
        if (!cfg.trafficByDay.empty() &&
            cfg.trafficByDay.size() != s.dayCount()) {
            throw std::invalid_argument(
                "trafficByDay must contain every day");
        }
        for (const auto& traffic : cfg.trafficByDay) {
            if (traffic.size() != s.field.cellCount()) {
                throw std::invalid_argument(
                    "daily traffic size must match field cells");
            }
            for (const TrafficStatus status : traffic) {
                if (status != TrafficStatus::Smooth &&
                    status != TrafficStatus::Busy &&
                    status != TrafficStatus::Jammed) {
                    throw std::invalid_argument(
                        "traffic status is invalid");
                }
            }
        }
    }

    static std::vector<TrafficStatus> trafficForDay(
        const MatchSetting& s,
        const MatchRunnerConfig& cfg,
        std::size_t day) {
        if (!cfg.trafficByDay.empty()) {
            return cfg.trafficByDay[day];
        }
        return std::vector<TrafficStatus>(
            s.field.cellCount(), TrafficStatus::Smooth);
    }

    static MatchState initialState(
        const MatchSetting& s,
        const std::vector<AgentKind>& kinds,
        std::vector<TrafficStatus> traffic) {
        MatchState st;
        st.day = 0;
        st.agents.reserve(kinds.size());
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            st.agents.push_back(
                Agent{i, kinds[i], s.agentStarts[i], s.fuelLimit});
        }
        st.trafficByPos = std::move(traffic);
        return st;
    }

    static std::int64_t endTime(
        const MatchSetting& s,
        std::size_t day) noexcept {
        if (s.startsAt <= 0) {
            return 0;
        }
        std::int64_t time = s.startsAt;
        for (std::size_t i = 0; i <= day; ++i) {
            if (time > std::numeric_limits<std::int64_t>::max() -
                           s.daySeconds[i]) {
                return std::numeric_limits<std::int64_t>::max();
            }
            time += s.daySeconds[i];
        }
        return time;
    }

    static DailyPlan waitPlan(
        std::size_t agents,
        std::int32_t steps) {
        return DailyPlan(agents, AgentPlan{-steps});
    }

    static MatchState nextState(
        const MatchSetting& s,
        const GameState& end,
        std::int32_t day,
        std::vector<TrafficStatus> traffic) {
        MatchState st;
        st.day = day;
        st.endsAt = endTime(s, static_cast<std::size_t>(day));
        st.trafficByPos = std::move(traffic);
        st.agents.reserve(end.agentCount);
        for (std::size_t i = 0; i < end.agentCount; ++i) {
            st.agents.push_back(end.agents[i]);
        }
        return st;
    }

public:
    static MatchRunResult run(
        const MatchSetting& s,
        const std::vector<AgentKind>& kinds,
        const MatchRunnerConfig& cfg = {}) {
        validate(s, kinds, cfg);

        MatchRunResult res;
        res.kinds = kinds;
        res.days.reserve(s.dayCount());

        MatchState st = initialState(
            s, kinds, trafficForDay(s, cfg, 0));
        st.endsAt = endTime(s, 0);
        ScoreState score;
        res.score = score;
        for (std::size_t day = 0; day < s.dayCount(); ++day) {
            MatchDayResult record;
            record.day = static_cast<std::int32_t>(day);
            record.startState = st;

            const BeamSearchResult search =
                brain1(s, st, score, cfg.beam);
            record.expandedNodes = search.expandedNodes;
            record.elapsedMs = search.elapsedMs;
            record.assignmentCacheHits = search.assignmentCacheHits;
            record.assignmentComputations = search.assignmentComputations;
            record.removedSupplyLoops = search.removedSupplyLoops;
            record.brandRepairAttempts = search.brandRepairAttempts;
            record.brandRepairExpanded = search.brandRepairExpanded;
            record.brandRepairImprovements = search.brandRepairImprovements;
            record.udonAlnsAttempts = search.udonAlnsAttempts;
            record.udonAlnsExpanded = search.udonAlnsExpanded;
            record.udonAlnsImprovements = search.udonAlnsImprovements;
            record.spotRouteLnsAttempts = search.spotRouteLnsAttempts;
            record.spotRouteLnsImprovements = search.spotRouteLnsImprovements;
            record.routeColumnsGenerated = search.routeColumnsGenerated;
            record.routeColumnsMasterNodes = search.routeColumnsMasterNodes;
            record.routeColumnsImprovements = search.routeColumnsImprovements;
            record.assignmentTimeNs = search.assignmentTimeNs;
            record.flowTimeNs = search.flowTimeNs;
            record.rendezvousTimeNs = search.rendezvousTimeNs;
            record.stopReason = search.stopReason;
            record.usedFallback = !search.success;
            if (search.success) {
                record.plan = search.plan;
                record.evaluation = search.evaluation;
            } else if (cfg.fallbackToWait) {
                record.plan = waitPlan(st.agents.size(), s.daySteps[day]);
            } else {
                res.failedDay = static_cast<std::int32_t>(day);
                return res;
            }

            const SimulationResult sim =
                simulateDay(s, st, record.plan, score);
            if (!sim.valid) {
                res.failedDay = static_cast<std::int32_t>(day);
                res.failure = sim.failure;
                return res;
            }

            record.endState = sim.state;
            record.roadStaySteps = sim.roadStaySteps;
            if (record.usedFallback) {
                EvaluationContext ctx;
                ctx.scoreAtDayStart = score;
                record.evaluation = evaluateGameState(
                    sim.state, ctx, cfg.beam.weights);
            }

            score = sim.state.score;
            res.score = score;
            res.days.push_back(std::move(record));

            if (day + 1 < s.dayCount()) {
                st = nextState(
                    s,
                    sim.state,
                    static_cast<std::int32_t>(day + 1),
                    trafficForDay(s, cfg, day + 1));
            }
        }

        res.success = true;
        return res;
    }
};

inline MatchRunResult runMatch(
    const MatchSetting& setting,
    const std::vector<AgentKind>& kinds,
    const MatchRunnerConfig& config = {}) {
    return MatchRunner::run(setting, kinds, config);
}

}  // namespace benzen
