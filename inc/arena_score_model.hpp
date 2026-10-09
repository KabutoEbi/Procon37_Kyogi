#pragma once

#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace benzen {

struct ArenaScorePrediction {
    std::int64_t targetUdon = 0;
    std::int32_t minimumPatrols = 0;
};

// #1833～#1927、49独立roundの公開首位玉数/全日在庫を線形回帰。
// #1928/#1930は未学習の検証用。再生成: tests/analyze_arena_history.py。
// 台数は最終得点だけでは一意に分からないので、取得上限による下限を返す。
inline ArenaScorePrediction predictArenaTopScore(const MatchSetting& s) {
    ArenaScorePrediction prediction;
    const auto size = std::max(s.field.width, s.field.height);
    if ((size != 16 && size != 24 && size != 32) ||
        s.field.width != s.field.height ||
        s.daySteps.empty() || s.field.spots.empty() || s.agentCount() == 0) {
        return prediction;
    }
    const double averageSteps = static_cast<double>(std::accumulate(
        s.daySteps.begin(), s.daySteps.end(), std::int64_t{0})) /
        static_cast<double>(s.dayCount());
    if (averageSteps <= 0.0) return prediction;
    const double duplicateShare = 1.0 -
        static_cast<double>(s.field.brands.size()) /
            static_cast<double>(s.field.spots.size());
    const double capture = std::clamp(
        0.23408271383648535 +
        0.09888879464413265 * averageSteps / size +
        0.03118590357397107 * s.fuelLimit / averageSteps +
        0.028997242544027125 * duplicateShare -
        (size == 24 ? 0.012077165971536321 : 0.0) -
        (size == 32 ? 0.0016673226289056677 : 0.0), 0.0, 1.0);
    std::int64_t stock = 0;
    for (const Spot& spot : s.field.spots) stock += spot.maxStock;
    prediction.targetUdon = std::max<std::int64_t>(
        static_cast<std::int64_t>(s.field.brands.size()) * s.dayCount(),
        static_cast<std::int64_t>(std::ceil(
            capture * stock * static_cast<double>(s.dayCount()))));
    prediction.minimumPatrols = static_cast<std::int32_t>(s.agentCount());
    for (std::int32_t patrols = 1;
         patrols <= static_cast<std::int32_t>(s.agentCount()); ++patrols) {
        std::int64_t capacity = 0;
        std::int64_t dailyStock = 0;
        for (const Spot& spot : s.field.spots)
            dailyStock += std::min(spot.maxStock, patrols);
        for (const auto steps : s.daySteps)
            capacity += std::min<std::int64_t>(dailyStock,
                static_cast<std::int64_t>(patrols) * steps);
        if (capacity >= prediction.targetUdon) {
            prediction.minimumPatrols = patrols;
            break;
        }
    }
    return prediction;
}

} // namespace benzen
