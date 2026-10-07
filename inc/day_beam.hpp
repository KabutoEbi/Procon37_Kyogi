#pragma once

#include "spot_route_lns.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace benzen {

struct DayPlanCandidate {
    DailyPlan plan;
    GameState state;
};

// Keep quantity, fuel, and position alternatives. Identical end states do not
// consume separate slots. The best immediate score is always retained.
inline void retainDayCandidates(std::vector<DayPlanCandidate>& candidates,
    std::size_t width, const MatchSetting& setting, const MatchState& today) {
    if (candidates.empty() || !width) { candidates.clear(); return; }
    std::vector<DijkstraTable> cache(setting.field.cellCount());
    std::vector<bool> cached(setting.field.cellCount(), false);
    std::vector<detail::FutureRoutePotential> future;
    for (const auto& c : candidates)
        future.push_back(detail::futureRoutePotential(setting, today, c.state, cache, cached));
    std::vector<DayPlanCandidate> kept;
    std::vector<bool> used(candidates.size(), false);
    for (std::size_t slot = 0; slot < width; ++slot) {
        std::size_t best = candidates.size();
        const auto rank = [&](std::size_t i) {
            const auto& s = candidates[i].state;
            const int quantity = s.score.totalUdon;
            const int placement = future[i].reachableBrands * 10000 - future[i].distanceSum;
            const int fuel = future[i].patrolFuel;
            return std::make_tuple(countBits(s.score.matchBrands), s.score.sumOfDailyBrandCounts,
                slot % 3 == 0 ? quantity : (slot % 3 == 1 ? placement : fuel),
                quantity, placement, fuel);
        };
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (used[i] || std::any_of(kept.begin(), kept.end(),
                [&](const auto& c) { return c.state == candidates[i].state; })) continue;
            if (best == candidates.size() || rank(i) > rank(best)) best = i;
        }
        if (best == candidates.size()) break;
        used[best] = true;
        kept.push_back(candidates[best]);
    }
    candidates = std::move(kept);
}

struct DayBeamResult {
    std::size_t selected = 0;
    std::size_t expanded = 0;
    bool complete = false;
};

// Each root receives a bounded scenario/depth/branch budget. Never compare a
// partly explored root with a completed one. Future traffic is a forecast,
// not hidden match data: use current traffic and a one-level worsening.
template<class Search>
inline DayBeamResult searchFutureDays(const MatchSetting& setting,
    const MatchState& today, const std::vector<DayPlanCandidate>& roots,
    std::size_t width, std::size_t depth,
    std::chrono::steady_clock::time_point deadline, Search search) {
    using Clock = std::chrono::steady_clock;
    DayBeamResult result;
    if (roots.empty() || !width || !depth || today.day < 0 ||
        static_cast<std::size_t>(today.day + 1) >= setting.dayCount()) return result;
    depth = std::min(depth, setting.dayCount() - static_cast<std::size_t>(today.day + 1));
    using Score = std::tuple<std::uint32_t, int, int>;
    Score best{};
    bool haveBest = false;
    for (std::size_t root = 0; root < roots.size(); ++root) {
        Score worst{std::numeric_limits<std::uint32_t>::max(),
                    std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
        for (int scenario = 0; scenario < 2; ++scenario) {
            if (Clock::now() >= deadline) return result;
            const auto slots = (roots.size() - root) * 2 - static_cast<std::size_t>(scenario);
            const auto scenarioDeadline = Clock::now() + (deadline - Clock::now()) / slots;
            std::vector<DayPlanCandidate> beam{roots[root]};
            for (std::size_t d = 0; d < depth; ++d) {
                std::vector<DayPlanCandidate> next;
                MatchState tomorrow;
                tomorrow.day = today.day + 1 + static_cast<int>(d);
                tomorrow.trafficByPos = today.trafficByPos;
                if (scenario == 1) for (auto& t : tomorrow.trafficByPos)
                    t = t == TrafficStatus::Smooth ? TrafficStatus::Busy : TrafficStatus::Jammed;
                const auto stageDeadline = Clock::now() +
                    (scenarioDeadline - Clock::now()) / (depth - d);
                std::size_t remaining = beam.size() * 3;
                for (const auto& parent : beam) for (int variant = 0; variant < 3; ++variant) {
                    if (Clock::now() >= stageDeadline) return result;
                    tomorrow.agents.assign(parent.state.agents.begin(),
                        parent.state.agents.begin() + parent.state.agentCount);
                    const auto callDeadline = Clock::now() +
                        (stageDeadline - Clock::now()) / remaining--;
                    auto child = search(tomorrow, parent.state.score, variant, callDeadline);
                    ++result.expanded;
                    if (!child.success) return result;
                    next.push_back({std::move(child.plan), child.finalState});
                }
                retainDayCandidates(next, width, setting, tomorrow);
                if (next.empty()) return result;
                beam = std::move(next);
            }
            Score scenarioBest{};
            for (const auto& candidate : beam) {
                const auto& s = candidate.state.score;
                scenarioBest = std::max(scenarioBest,
                    Score{countBits(s.matchBrands), s.sumOfDailyBrandCounts, s.totalUdon});
            }
            worst = std::min(worst, scenarioBest);
        }
        if (!haveBest || worst > best) {
            best = worst;
            haveBest = true;
            result.selected = root;
        }
    }
    result.complete = true;
    return result;
}

} // namespace benzen
