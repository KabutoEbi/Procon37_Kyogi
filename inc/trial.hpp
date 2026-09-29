#pragma once

#include "brain/brain1.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace benzen {

using TrialTraffic = std::vector<std::vector<TrafficStatus>>;
using TrialScore = std::array<std::int64_t, 3>;
using TrialDeadline = std::chrono::steady_clock::time_point;

inline constexpr TrialDeadline kNoTrialDeadline = TrialDeadline::max();

inline TrialScore trialScore(const ScoreState& score) {
    return {countBits(score.matchBrands), score.sumOfDailyBrandCounts, score.totalUdon};
}

struct TrialResult {
    bool success = false;
    ScoreState score{};
    EvaluationBreakdown evaluation{};
    std::size_t days = 0;
};

namespace trial_detail {

inline bool applyDeadline(
    BeamSearchConfig& beam, TrialDeadline deadline,
    std::size_t remainingDays) {
    if (deadline == kNoTrialDeadline) return true;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0 || remainingDays == 0) return false;
    const auto dailyMs = std::max<std::int64_t>(
        1, left / static_cast<std::int64_t>(remainingDays));
    beam.timeLimitMs = beam.timeLimitMs > 0
        ? std::min(beam.timeLimitMs, dailyMs) : dailyMs;
    beam.safetyMarginMs = 0;
    return true;
}

}  // namespace trial_detail

// オフライン試走。各日の探索には当日分の道路状態だけを渡す。
inline TrialResult runTrial(
    const MatchSetting& setting, const std::vector<AgentKind>& kinds,
    const TrialTraffic& traffic, BeamSearchConfig beam,
    std::size_t dayLimit = 0,
    TrialDeadline deadline = kNoTrialDeadline) {
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
        auto dailyBeam = beam;
        if (!trial_detail::applyDeadline(dailyBeam, deadline, days - day))
            return result;
        const auto answer = brain1(setting, state, result.score, dailyBeam);
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

struct PredictedRoadLoad {
    double meanPerTeam = 0.0;
    double deviationPerTeam = 0.0;
};

// 全チームのエージェント滞在を道路へ分配したときの、1道路セルあたりの
// チーム平均交通量を見積もる。道路は移動経路に選ばれやすいため、他の
// 通行可能セルの2倍の重みを持たせる。
inline PredictedRoadLoad predictRoadLoad(
    const MatchSetting& setting, std::size_t day) noexcept {
    if (day == 0 || day >= setting.dayCount() ||
        setting.playerCount <= 0 || setting.agentCount() == 0) {
        return {};
    }

    double weightedCells = 0.0;
    for (const Cell& cell : setting.field.cells) {
        if (!cell.isPassable()) continue;
        weightedCells += cell.type == CellType::Road ? 2.0 : 1.0;
    }
    if (weightedCells <= 0.0) return {};

    std::int64_t historySteps = setting.daySteps[day - 1];
    if (day >= 2) historySteps += setting.daySteps[day - 2];
    if (historySteps <= 0) return {};

    const double teams = static_cast<double>(setting.playerCount);
    const double trials = teams * static_cast<double>(setting.agentCount()) *
                          static_cast<double>(historySteps);
    const double roadProbability = std::min(1.0, 2.0 / weightedCells);
    PredictedRoadLoad load;
    load.meanPerTeam = trials * roadProbability / teams;
    load.deviationPerTeam = std::sqrt(
        trials * roadProbability * (1.0 - roadProbability)) / teams;
    return load;
}

inline TrafficStatus predictedTrafficStatus(
    const MatchSetting& setting, double load) noexcept {
    const std::int32_t busy = std::max(1, setting.busyThreshold);
    const std::int32_t jammed = std::max(busy + 1, setting.jammedThreshold);
    if (load >= static_cast<double>(jammed)) return TrafficStatus::Jammed;
    if (load >= static_cast<double>(busy)) return TrafficStatus::Busy;
    return TrafficStatus::Smooth;
}

// 初日は必ず全道路が順調。以後はチーム数・エージェント数・直前の
// ステップ数から予測した低位／中央／高位の交通条件を比較する。
inline std::vector<TrialTraffic> makeTrialScenarios(
    const MatchSetting& setting, std::size_t count = 3) {
    auto spatialDeviation = [](std::size_t day, std::size_t pos) noexcept {
        std::uint64_t value =
            0x9e3779b97f4a7c15ULL ^
            (static_cast<std::uint64_t>(day) * 0x94d049bb133111ebULL) ^
            static_cast<std::uint64_t>(pos);
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31;
        const double unit = static_cast<double>(value >> 53) /
                            static_cast<double>(std::uint64_t{1} << 11);
        return (unit - 0.5) * 1.5;
    };
    std::vector<TrialTraffic> scenarios;
    for (std::size_t scenario = 0; scenario < count; ++scenario) {
        TrialTraffic traffic(setting.dayCount(), std::vector<TrafficStatus>(
            setting.field.cellCount(), TrafficStatus::Smooth));
        const double scenarioDeviation = count <= 1
            ? 0.0
            : -1.0 + 2.0 * static_cast<double>(scenario) /
                         static_cast<double>(count - 1);
        for (std::size_t day = 1; day < traffic.size(); ++day) {
            const PredictedRoadLoad prediction = predictRoadLoad(setting, day);
            for (std::size_t pos = 0; pos < traffic[day].size(); ++pos) {
                if (setting.field.cells[pos].type != CellType::Road) continue;
                const double load = std::max(
                    0.0,
                    prediction.meanPerTeam + prediction.deviationPerTeam *
                        (scenarioDeviation + spatialDeviation(day, pos)));
                traffic[day][pos] = predictedTrafficStatus(setting, load);
            }
        }
        if (std::find(scenarios.begin(), scenarios.end(), traffic) == scenarios.end())
            scenarios.push_back(std::move(traffic));
    }
    return scenarios;
}

}  // namespace benzen
