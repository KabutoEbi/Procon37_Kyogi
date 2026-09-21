#pragma once

#include "trial.hpp"

#include <cmath>
#include <ostream>
#include <random>
#include <set>
#include <string>

namespace procon37 {

struct WeightTuningCase {
    std::uint64_t id = 0; // 学習・検証で同じ問題を使わないための識別子。
    MatchSetting setting;
    std::vector<AgentKind> kinds;
    TrialTraffic traffic;
};

struct WeightTuningConfig {
    std::size_t candidates = 24;
    std::size_t trialDays = 0;
    std::uint64_t seed = 1;
    double scale = 1.5;
    BeamSearchConfig beam{};
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

inline TrialScore evaluateWeights(
    const std::vector<WeightTuningCase>& cases, const EvaluationWeights& weights,
    const WeightTuningConfig& config) {
    TrialScore score{};
    auto beam = config.beam;
    beam.weights = weights;
    beam.timeLimitMs = 0;
    beam.useStateDeadline = false;
    beam.threadCount = 1; // 同一の探索予算による再現可能な比較。
    for (const auto& problem : cases) {
        const auto trial = runTrial(problem.setting, problem.kinds, problem.traffic,
                                    beam, config.trialDays);
        if (!trial.success) throw std::runtime_error("weight tuning trial failed");
        const auto value = trialScore(trial.score);
        for (std::size_t k = 0; k < score.size(); ++k) score[k] += value[k];
    }
    return score;
}

// 固定seedの乗法的座標探索。候補選択は学習集合だけで行い、最後に一度
// 検証集合を評価する。検証スコアが初期値未満なら初期重みへ戻す。
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
    result.trainingBefore = evaluateWeights(training, initial, config);
    result.validationBefore = evaluateWeights(validation, initial, config);
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
        const auto score = evaluateWeights(training, candidate, config);
        ++result.evaluatedCandidates;
        if (score > result.trainingAfter) {
            result.weights = candidate;
            result.trainingAfter = score;
        }
    }
    if (result.trainingAfter > result.trainingBefore) {
        result.validationAfter = evaluateWeights(validation, result.weights, config);
        result.accepted = result.validationAfter >= result.validationBefore;
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
    out << "procon37::EvaluationWeights weights;\n";
    out << "weights.newBrand = " << weights.newBrand << ";\n"
        << "weights.dailyBrand = " << weights.dailyBrand << ";\n"
        << "weights.udon = " << weights.udon << ";\n"
        << "weights.concentration = " << weights.concentration << ";\n";
    for (const auto& parameter : kTunableWeights)
        out << "weights." << parameter.name << " = "
            << weights.*(parameter.member) << ";\n";
}

}  // namespace procon37
