#pragma once

#include "trial.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <ostream>
#include <random>
#include <set>
#include <string>

namespace benzen {

struct WeightTuningCase {
    std::uint64_t id = 0; // 学習・検証で同じ問題を使わないための識別子。
    MatchSetting setting;
    std::vector<AgentKind> kinds;
    TrialTraffic traffic;
    std::uint64_t group = 0; // 交通・燃料等の条件別にも退行を検査する。
};

struct WeightTuningConfig {
    std::size_t candidates = 24;
    std::size_t trialDays = 0;
    std::uint64_t seed = 1;
    double scale = 1.5;
    BeamSearchConfig beam{};
    bool fixedWork = true; // falseならbeam.timeLimitMsで実運用の短時間評価を行う。
};

struct WeightTuningResult {
    EvaluationWeights weights{};
    TrialScore trainingBefore{}, trainingAfter{};
    TrialScore validationBefore{}, validationAfter{};
    std::size_t evaluatedCandidates = 0;
    bool accepted = false;
};

struct WeightParameter {
    const char* name;
    double EvaluationWeights::*member;
};

// 公式の得点優先順位を固定し、探索誘導と翌日価値だけを調整する。
inline constexpr std::array<WeightParameter, 15> kTunableWeights = {{
    {"remainingFuel", &EvaluationWeights::remainingFuel},
    {"unusedStep", &EvaluationWeights::unusedStep},
    {"valuableSpotDistance", &EvaluationWeights::valuableSpotDistance},
    {"newBrandSpotDistance", &EvaluationWeights::newBrandSpotDistance},
    {"dailyBrandSpotDistance", &EvaluationWeights::dailyBrandSpotDistance},
    {"targetSlack", &EvaluationWeights::targetSlack},
    {"supplyDistance", &EvaluationWeights::supplyDistance},
    {"rendezvousMismatch", &EvaluationWeights::rendezvousMismatch},
    {"fuelRisk", &EvaluationWeights::fuelRisk},
    {"unreachableTarget", &EvaluationWeights::unreachableTarget},
    {"nextDayNewBrand", &EvaluationWeights::nextDayNewBrand},
    {"nextDayBrand", &EvaluationWeights::nextDayBrand},
    {"nextDayUdon", &EvaluationWeights::nextDayUdon},
    {"nextDayNewBrandApproach", &EvaluationWeights::nextDayNewBrandApproach},
    {"nextDayBrandApproach", &EvaluationWeights::nextDayBrandApproach},
}};

inline std::map<std::uint64_t, TrialScore> evaluateWeightGroups(
    const std::vector<WeightTuningCase>& cases, const EvaluationWeights& weights,
    const WeightTuningConfig& config) {
    std::map<std::uint64_t, TrialScore> scores;
    auto beam = config.beam;
    beam.weights = weights;
    if (config.fixedWork) beam.timeLimitMs = 0;
    else if (beam.timeLimitMs <= 0)
        throw std::invalid_argument("timed weight evaluation requires a positive time limit");
    beam.safetyMarginMs = 0;
    beam.useStateDeadline = false;
    beam.threadCount = 1; // 固定探索量なら再現可能。実時間評価には計測変動がある。
    for (const auto& problem : cases) {
        const auto trial = runTrial(problem.setting, problem.kinds, problem.traffic,
                                    beam, config.trialDays);
        if (!trial.success) throw std::runtime_error("weight tuning trial failed");
        const auto value = trialScore(trial.score);
        auto& score = scores[problem.group];
        for (std::size_t k = 0; k < score.size(); ++k) score[k] += value[k];
    }
    return scores;
}

inline TrialScore sumWeightScores(const std::map<std::uint64_t, TrialScore>& groups) {
    TrialScore sum{};
    for (const auto& entry : groups)
        for (std::size_t k = 0; k < sum.size(); ++k) sum[k] += entry.second[k];
    return sum;
}

inline bool weightGroupsNotWorse(
    const std::map<std::uint64_t, TrialScore>& baseline,
    const std::map<std::uint64_t, TrialScore>& candidate) {
    for (const auto& entry : baseline) {
        const auto found = candidate.find(entry.first);
        if (found == candidate.end() || found->second < entry.second) return false;
    }
    return true;
}

inline TrialScore evaluateWeights(
    const std::vector<WeightTuningCase>& cases, const EvaluationWeights& weights,
    const WeightTuningConfig& config) {
    return sumWeightScores(evaluateWeightGroups(cases, weights, config));
}

// 固定seedの乗法的座標探索。候補選択は学習集合だけで行い、最後に一度
// 検証集合を評価する。いずれかの条件群が初期値未満なら初期重みへ戻す。
inline WeightTuningResult tuneWeights(
    const std::vector<WeightTuningCase>& training,
    const std::vector<WeightTuningCase>& validation,
    const EvaluationWeights& initial = {}, const WeightTuningConfig& config = {}) {
    if (training.empty() || validation.empty() || !std::isfinite(config.scale) ||
        config.scale <= 1.0 || config.scale > 4.0)
        throw std::invalid_argument("tuning requires train/validation cases and scale in (1,4]");
    std::set<std::uint64_t> ids;
    for (const auto* set : {&training, &validation})
        for (const auto& problem : *set)
            if (!ids.insert(problem.id).second)
                throw std::invalid_argument("duplicate training/validation case id");
    WeightTuningResult result;
    result.weights = initial;
    const auto trainingBaseline = evaluateWeightGroups(training, initial, config);
    result.trainingBefore = sumWeightScores(trainingBaseline);
    const auto validationBaseline = evaluateWeightGroups(validation, initial, config);
    result.validationBefore = sumWeightScores(validationBaseline);
    result.trainingAfter = result.trainingBefore;
    result.validationAfter = result.validationBefore;
    std::mt19937_64 random(config.seed);
    std::vector<std::size_t> dimensions(kTunableWeights.size());
    for (std::size_t i = 0; i < dimensions.size(); ++i) dimensions[i] = i;
    // 同一座標の増減を対にして比較し、seedで探索する座標順だけを変える。
    for (std::size_t i = dimensions.size(); i > 1; --i)
        std::swap(dimensions[i - 1], dimensions[random() % i]);
    EvaluationWeights center = initial;
    for (std::size_t attempt = 0; attempt < config.candidates; ++attempt) {
        if (attempt % 2 == 0) center = result.weights;
        auto candidate = center;
        const auto member = kTunableWeights[
            dimensions[(attempt / 2) % dimensions.size()]].member;
        const double factor = attempt % 2 == 0 ? config.scale : 1.0 / config.scale;
        candidate.*member = std::clamp(center.*member * factor,
            std::min(initial.*member / 4.0, initial.*member * 4.0),
            std::max(initial.*member / 4.0, initial.*member * 4.0));
        const auto groups = evaluateWeightGroups(training, candidate, config);
        const auto score = sumWeightScores(groups);
        ++result.evaluatedCandidates;
        if (score > result.trainingAfter && weightGroupsNotWorse(trainingBaseline, groups)) {
            result.weights = candidate;
            result.trainingAfter = score;
        }
    }
    if (result.trainingAfter > result.trainingBefore) {
        const auto groups = evaluateWeightGroups(validation, result.weights, config);
        result.validationAfter = sumWeightScores(groups);
        result.accepted = weightGroupsNotWorse(validationBaseline, groups);
        if (!result.accepted) {
            result.weights = initial;
            result.trainingAfter = result.trainingBefore;
            result.validationAfter = result.validationBefore;
        }
    }
    return result;
}

inline void printTunedWeights(std::ostream& out, const EvaluationWeights& weights) {
    out.precision(17);
    out << "benzen::EvaluationWeights weights;\n";
    out << "weights.newBrand = " << weights.newBrand << ";\n"
        << "weights.dailyBrand = " << weights.dailyBrand << ";\n"
        << "weights.matchBrandCompletion = "
        << weights.matchBrandCompletion << ";\n"
        << "weights.dailyBrandCompletion = "
        << weights.dailyBrandCompletion << ";\n"
        << "weights.udon = " << weights.udon << ";\n"
        << "weights.concentration = " << weights.concentration << ";\n";
    for (const auto& parameter : kTunableWeights)
        out << "weights." << parameter.name << " = "
            << weights.*(parameter.member) << ";\n";
}

}  // namespace benzen
