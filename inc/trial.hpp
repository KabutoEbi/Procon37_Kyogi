#pragma once

#include "brain/brain1.hpp"

#include <array>
#include <stdexcept>
#include <vector>

namespace benzen {

using TrialTraffic = std::vector<std::vector<TrafficStatus>>;
using TrialScore = std::array<std::int64_t, 3>;

inline TrialScore trialScore(const ScoreState& score) {
    return {countBits(score.matchBrands), score.sumOfDailyBrandCounts, score.totalUdon};
}

struct TrialResult {
    bool success = false;
    ScoreState score{};
    EvaluationBreakdown evaluation{};
    std::size_t days = 0;
};

// オフライン試走。各日の探索には当日分の道路状態だけを渡す。
inline TrialResult runTrial(
    const MatchSetting& setting, const std::vector<AgentKind>& kinds,
    const TrialTraffic& traffic, BeamSearchConfig beam,
    std::size_t dayLimit = 0) {
    TrialResult result;
    const std::size_t days = dayLimit == 0 ? setting.dayCount()
                                         : std::min(dayLimit, setting.dayCount());
    if (days == 0 || kinds.size() != setting.agentCount()) return result;
    if (!traffic.empty()) {
        if (traffic.size() < days) throw std::invalid_argument("missing trial traffic day");
        for (std::size_t day = 0; day < days; ++day) {
            if (traffic[day].size() != setting.field.cellCount())
                throw std::invalid_argument("trial traffic size mismatch");
            for (const auto status : traffic[day])
                if (status != TrafficStatus::Smooth && status != TrafficStatus::Busy &&
                    status != TrafficStatus::Jammed)
                    throw std::invalid_argument("invalid trial traffic status");
        }
    }
    MatchState state;
    for (AgentIndex a = 0; a < kinds.size(); ++a)
        state.agents.push_back({a, kinds[a], setting.agentStarts[a], setting.fuelLimit});
    beam.useStateDeadline = false;
    for (std::size_t day = 0; day < days; ++day) {
        state.day = static_cast<std::int32_t>(day);
        state.trafficByPos = traffic.empty()
            ? std::vector<TrafficStatus>(setting.field.cellCount(), TrafficStatus::Smooth)
            : traffic[day];
        const auto answer = brain1(setting, state, result.score, beam);
        if (!answer.success) return result;
        const auto sim = simulateDay(setting, state, answer.plan, result.score);
        if (!sim.valid) return result;
        result.score = sim.state.score;
        result.evaluation = answer.evaluation;
        state.agents.assign(sim.state.agents.begin(),
                            sim.state.agents.begin() + sim.state.agentCount);
        ++result.days;
    }
    result.evaluation.newBrandCount = static_cast<std::int32_t>(countBits(result.score.matchBrands));
    result.evaluation.dailyBrandCount = result.score.sumOfDailyBrandCounts;
    result.evaluation.udonCount = result.score.totalUdon;
    result.success = true;
    return result;
}

// 予測値ではなく種別比較の固定テスト条件。全順調と2種類の混合交通。
inline std::vector<TrialTraffic> makeTrialScenarios(
    const MatchSetting& setting, std::size_t count = 3) {
    std::vector<TrialTraffic> scenarios;
    for (std::size_t scenario = 0; scenario < count; ++scenario) {
        TrialTraffic traffic(setting.dayCount(), std::vector<TrafficStatus>(
            setting.field.cellCount(), TrafficStatus::Smooth));
        if (scenario > 0)
            for (std::size_t day = 0; day < traffic.size(); ++day)
                for (std::size_t pos = 0; pos < traffic[day].size(); ++pos)
                    if (setting.field.cells[pos].type == CellType::Road)
                        traffic[day][pos] = static_cast<TrafficStatus>(
                            (pos * 17 + day * 7 + scenario) % 3);
        if (std::find(scenarios.begin(), scenarios.end(), traffic) == scenarios.end())
            scenarios.push_back(std::move(traffic));
    }
    return scenarios;
}

}  // namespace benzen
