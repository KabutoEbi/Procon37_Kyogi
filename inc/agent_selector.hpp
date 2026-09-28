#pragma once

#include "brain/brain0.hpp"
#include "brain/brain1.hpp"
#include "trial.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace benzen {

struct AgentSelectorConfig {
    // 既定では燃料切れで全車が行動不能になるのを避けるため、
    // 補給車を最低1台残す。-1なら「全車が補給車」だけを除いた
    // 最大台数まで調べる。
    std::int32_t minSupplyCount = 1;
    std::int32_t maxSupplyCount = -1;

    // 距離評価後にbrain1で試走する候補数。0なら距離評価だけを使う。
    std::size_t trialCandidateLimit = 8;
    std::size_t trialBeamWidth = 32;
    std::size_t trialJointBeamWidth = 128;
    EvaluationWeights trialWeights{};
    std::size_t trialDays = 0; // 0なら全日。実際の各日のステップ数を使う。
    std::size_t trialScenarioCount = 3;
    std::vector<TrialTraffic> trialScenarios;

    // 0なら制限なし。相対制限とUNIX時刻の短い方を使う。
    std::int64_t timeLimitMs = 0;
    std::int64_t deadlineUnixSeconds = 0;
    std::int64_t safetyMarginMs = 1000;
};

struct AgentSelectionResult {
    bool success = false;
    std::vector<AgentKind> kinds;
    std::int32_t supplyCount = 0;
    double heuristicScore = -std::numeric_limits<double>::infinity();
    bool usedBeamTrial = false;
    EvaluationBreakdown trialEvaluation{};
    std::size_t evaluatedAssignments = 0;
    std::size_t trialAssignments = 0;
    std::size_t trialSimulations = 0;
    bool timedOut = false;
    TrialScore worstTrialScore{};
    TrialScore totalTrialScore{};
};

class AgentSelector final {
private:
    struct Candidate {
        std::vector<AgentKind> kinds;
        std::int32_t supplyCount = 0;
        double heuristicScore = 0.0;
        std::uint32_t supplyMask = 0;
        std::int64_t supplySpread = 0;
    };

    static void validate(
        const MatchSetting& s,
        const AgentSelectorConfig& cfg) {
        const std::size_t n = s.agentCount();
        if (n == 0 || n > kMaxAgents) {
            throw std::invalid_argument(
                "agent count must be in [1, kMaxAgents]");
        }
        if (s.daySteps.empty() || !s.hasConsistentDaySettings() ||
            s.fuelLimit <= 0 ||
            s.field.cells.size() != s.field.cellCount()) {
            throw std::invalid_argument("match setting is incomplete");
        }
        for (const CellPos start : s.agentStarts) {
            if (!s.field.contains(start) ||
                !s.field.at(start).isPassable()) {
                throw std::invalid_argument(
                    "every agent start must be passable");
            }
        }

        const std::int32_t maximum =
            cfg.maxSupplyCount < 0
                ? static_cast<std::int32_t>(n) - 1
                : cfg.maxSupplyCount;
        if (cfg.minSupplyCount < 0 || cfg.minSupplyCount > maximum ||
            maximum >= static_cast<std::int32_t>(n)) {
            throw std::invalid_argument(
                "supply count range must leave at least one patrol agent");
        }
        if (cfg.trialCandidateLimit > 0 &&
            (cfg.trialBeamWidth == 0 || cfg.trialJointBeamWidth == 0)) {
            throw std::invalid_argument(
                "beam widths must be positive when trials are enabled");
        }
        if (cfg.timeLimitMs < 0 || cfg.deadlineUnixSeconds < 0 ||
            cfg.safetyMarginMs < 0) {
            throw std::invalid_argument("selection deadline is invalid");
        }
    }


    static std::int32_t countBits(std::uint32_t value) noexcept {
        std::int32_t count = 0;
        while (value != 0) {
            value &= value - 1;
            ++count;
        }
        return count;
    }

    static std::int32_t distanceTo(
        const DijkstraTable& table,
        CellPos pos) noexcept {
        if (pos < 0 ||
            static_cast<std::size_t>(pos) >= table.distances.size()) {
            return kImpassableCost;
        }
        return table.distances[static_cast<std::size_t>(pos)];
    }

    static bool betterHeuristic(
        const Candidate& lhs,
        const Candidate& rhs) noexcept {
        if (lhs.heuristicScore != rhs.heuristicScore) {
            return lhs.heuristicScore > rhs.heuristicScore;
        }
        if (lhs.supplyCount != rhs.supplyCount) {
            return lhs.supplyCount < rhs.supplyCount;
        }
        return lhs.supplyMask < rhs.supplyMask;
    }

    static std::vector<AgentKind> fallbackKinds(
        std::size_t agentCount,
        std::int32_t minSupplyCount) {
        std::vector<AgentKind> kinds(agentCount, AgentKind::Patrol);
        for (std::int32_t i = 0; i < minSupplyCount; ++i) {
            kinds[static_cast<std::size_t>(i)] = AgentKind::Supply;
        }
        return kinds;
    }

public:
    static AgentSelectionResult select(
        const MatchSetting& s,
        const AgentSelectorConfig& cfg = {}) {
        validate(s, cfg);

        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        std::int64_t budgetMs = cfg.timeLimitMs > 0
                                    ? cfg.timeLimitMs
                                    : std::numeric_limits<std::int64_t>::max();
        if (cfg.deadlineUnixSeconds > 0) {
            const auto deadline = std::chrono::system_clock::time_point{
                std::chrono::seconds(cfg.deadlineUnixSeconds)};
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::system_clock::now()).count();
            budgetMs = std::min(budgetMs, left);
        }
        const bool limited =
            budgetMs != std::numeric_limits<std::int64_t>::max();
        if (limited) {
            budgetMs = std::max<std::int64_t>(
                0, budgetMs - cfg.safetyMarginMs);
        }
        const auto deadline = limited
                                  ? started + std::chrono::milliseconds(budgetMs)
                                  : Clock::time_point::max();
        auto expired = [&]() noexcept {
            return limited && Clock::now() >= deadline;
        };

        const std::size_t n = s.agentCount();
        const std::int32_t maxSupply =
            cfg.maxSupplyCount < 0
                ? static_cast<std::int32_t>(n) - 1
                : cfg.maxSupplyCount;
        const std::int32_t steps = s.daySteps.front();
        const std::uint32_t masks =
            std::uint32_t{1} << static_cast<std::uint32_t>(n);

        MatchState base;
        base.day = 0;
        base.trafficByPos.assign(s.field.cellCount(), TrafficStatus::Smooth);
        std::vector<DijkstraTable> dist;
        dist.reserve(n);
        for (const CellPos start : s.agentStarts) {
            if (expired()) {
                AgentSelectionResult fallback;
                fallback.success = true;
                fallback.timedOut = true;
                fallback.kinds = fallbackKinds(n, cfg.minSupplyCount);
                fallback.supplyCount = cfg.minSupplyCount;
                return fallback;
            }
            dist.push_back(Brain0::dijkstra(
                s, base, start, [&]() { return expired(); }));
        }

        std::vector<Candidate> cs;
        for (std::uint32_t mask = 0; mask < masks; ++mask) {
            if (expired()) {
                break;
            }
            const std::int32_t supplyN = countBits(mask);
            if (supplyN < cfg.minSupplyCount || supplyN > maxSupply) {
                continue;
            }

            Candidate c;
            c.supplyCount = supplyN;
            c.supplyMask = mask;
            c.kinds.resize(n, AgentKind::Patrol);
            for (std::size_t i = 0; i < n; ++i) {
                if ((mask & (std::uint32_t{1} << i)) != 0) {
                    c.kinds[i] = AgentKind::Supply;
                }
            }

            std::vector<bool> brands(s.field.brands.size(), false);
            std::int32_t spots = 0;
            std::int32_t stock = 0;
            std::int64_t distSum = 0;
            for (const Spot& spot : s.field.spots) {
                std::int32_t near = kImpassableCost;
                for (std::size_t i = 0; i < n; ++i) {
                    if (c.kinds[i] != AgentKind::Patrol) {
                        continue;
                    }
                    near = std::min(near, distanceTo(dist[i], spot.pos));
                }
                if (near > steps || near >= kImpassableCost) {
                    continue;
                }
                ++spots;
                stock += spot.maxStock;
                distSum += near;
                if (spot.brandIndex >= 0 &&
                    static_cast<std::size_t>(spot.brandIndex) <
                        brands.size()) {
                    brands[static_cast<std::size_t>(spot.brandIndex)] = true;
                }
            }

            const auto brandN = static_cast<std::int32_t>(
                std::count(brands.begin(), brands.end(), true));

            // 到達可能なスポットの和集合だけでは、巡回車1台で全地点に
            // 届く配置と、複数台で並行して回れる配置を区別できない。
            // 各巡回車が初日に担当できそうな近い3地点を見積もる。
            double patrolCapacity = 0.0;
            for (std::size_t p = 0; p < n; ++p) {
                if (c.kinds[p] != AgentKind::Patrol) continue;
                std::array<double, 3> nearest{};
                for (const Spot& spot : s.field.spots) {
                    const std::int32_t d = distanceTo(dist[p], spot.pos);
                    if (d > steps || d >= kImpassableCost) continue;
                    const double value =
                        static_cast<double>(steps + 1 - d) /
                        static_cast<double>(steps + 1);
                    if (value > nearest[0]) {
                        nearest[2] = nearest[1];
                        nearest[1] = nearest[0];
                        nearest[0] = value;
                    } else if (value > nearest[1]) {
                        nearest[2] = nearest[1];
                        nearest[1] = value;
                    } else if (value > nearest[2]) {
                        nearest[2] = value;
                    }
                }
                patrolCapacity += nearest[0] +
                    0.5 * nearest[1] + 0.25 * nearest[2];
            }

            // 同じ補給車台数なら、巡回車と主要スポットに近い開始位置を
            // 補給車へ割り当てる。燃料が少ない問題ほど影響を強める。
            std::int64_t supplyDist = 0;
            if (supplyN > 0) {
                for (std::size_t p = 0; p < n; ++p) {
                    if (c.kinds[p] != AgentKind::Patrol) {
                        continue;
                    }
                    std::int32_t near = kImpassableCost;
                    for (std::size_t q = 0; q < n; ++q) {
                        if (c.kinds[q] != AgentKind::Supply) {
                            continue;
                        }
                        near = std::min(
                            near, distanceTo(dist[q], s.agentStarts[p]));
                    }
                    if (near < kImpassableCost) {
                        supplyDist += near;
                    }
                }
            }
            for (std::size_t p = 0; p < n; ++p) {
                if (c.kinds[p] != AgentKind::Supply) continue;
                for (std::size_t q = p + 1; q < n; ++q) {
                    if (c.kinds[q] != AgentKind::Supply) continue;
                    const auto d = distanceTo(dist[p], s.agentStarts[q]);
                    if (d < kImpassableCost) c.supplySpread += d;
                }
            }

            const double maxFuel =
                3.0 * static_cast<double>(std::max(1, steps));
            const double fuelPressure = std::max(
                0.0,
                (maxFuel - s.fuelLimit) / maxFuel);
            c.heuristicScore =
                1.0e9 * brandN + 1.0e6 * spots + 1.0e4 * stock -
                100.0 * static_cast<double>(distSum) -
                10.0 * fuelPressure * static_cast<double>(supplyDist) +
                1.0e5 * patrolCapacity;
            cs.push_back(std::move(c));
        }

        std::sort(cs.begin(), cs.end(), betterHeuristic);
        AgentSelectionResult res;
        res.evaluatedAssignments = cs.size();
        if (cs.empty()) {
            if (expired()) {
                res.success = true;
                res.timedOut = true;
                res.kinds = fallbackKinds(n, cfg.minSupplyCount);
                res.supplyCount = cfg.minSupplyCount;
            }
            return res;
        }

        std::vector<std::size_t> trials;
        if (cfg.trialCandidateLimit > 0) {
            // まず補給車台数ごとの最良候補を比較する。
            for (std::int32_t count = cfg.minSupplyCount;
                 count <= maxSupply && trials.size() < cfg.trialCandidateLimit;
                 ++count) {
                const auto found = std::find_if(cs.begin(), cs.end(),
                    [count](const Candidate& c) {
                        return c.supplyCount == count;
                    });
                if (found != cs.end())
                    trials.push_back(static_cast<std::size_t>(found - cs.begin()));
            }
            // 同じ最小補給台数でも位置が違えば結果は大きく変わる。
            // 多台数の広域配置を試す前に、その次善案を1つ確保する。
            for (std::size_t i = 0; i < cs.size() &&
                 trials.size() < cfg.trialCandidateLimit; ++i) {
                if (cs[i].supplyCount != cfg.minSupplyCount ||
                    std::find(trials.begin(), trials.end(), i) != trials.end())
                    continue;
                trials.push_back(i);
                break;
            }
            // 残りの枠では補給車を広く置く候補も試す。
            for (std::int32_t count = std::max<std::int32_t>(2, cfg.minSupplyCount);
                 count <= maxSupply && trials.size() < cfg.trialCandidateLimit;
                 ++count) {
                std::size_t spread = cs.size();
                for (std::size_t i = 0; i < cs.size(); ++i) {
                    if (cs[i].supplyCount != count ||
                        std::find(trials.begin(), trials.end(), i) != trials.end())
                        continue;
                    if (spread == cs.size() ||
                        cs[i].supplySpread > cs[spread].supplySpread)
                        spread = i;
                }
                if (spread != cs.size()) trials.push_back(spread);
            }
            for (std::size_t i = 0;
                 i < cs.size() && trials.size() < cfg.trialCandidateLimit; ++i) {
                if (std::find(trials.begin(), trials.end(), i) == trials.end())
                    trials.push_back(i);
            }
        }

        const Candidate* best = &cs.front();
        EvaluationBreakdown bestEval{};
        TrialScore bestWorst{}, bestTotal{};
        bool tried = false;
        BeamSearchConfig beam;
        beam.beamWidth = cfg.trialBeamWidth;
        beam.jointBeamWidth = cfg.trialJointBeamWidth;
        beam.weights = cfg.trialWeights;
        beam.threadCount = 1;
        beam.useStateDeadline = false;
        auto scenarios = cfg.trialScenarios.empty()
            ? makeTrialScenarios(s, cfg.trialScenarioCount)
            : cfg.trialScenarios;
        if (cfg.trialCandidateLimit > 0 && scenarios.empty())
            throw std::invalid_argument("at least one trial scenario is required");
        for (const std::size_t i : trials) {
            if (expired()) {
                res.timedOut = true;
                break;
            }
            const Candidate& c = cs[i];
            TrialScore worst;
            worst.fill(std::numeric_limits<std::int64_t>::max());
            TrialScore total{};
            EvaluationBreakdown worstEval{};
            bool valid = true;
            for (const auto& traffic : scenarios) {
                if (expired()) {
                    res.timedOut = true;
                    valid = false;
                    break;
                }
                BeamSearchConfig trialBeam = beam;
                if (limited) {
                    const std::int64_t remaining =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - Clock::now()).count();
                    const std::size_t trialDayCount = cfg.trialDays == 0
                        ? s.dayCount()
                        : std::min(cfg.trialDays, s.dayCount());
                    trialBeam.timeLimitMs = std::max<std::int64_t>(
                        1, remaining /
                               static_cast<std::int64_t>(
                                   std::max<std::size_t>(1, trialDayCount)));
                    trialBeam.safetyMarginMs = 0;
                }
                const auto trial = runTrial(
                    s, c.kinds, traffic, trialBeam, cfg.trialDays);
                ++res.trialSimulations;
                if (!trial.success) { valid = false; break; }
                const auto score = trialScore(trial.score);
                if (score < worst) { worst = score; worstEval = trial.evaluation; }
                for (std::size_t k = 0; k < total.size(); ++k) total[k] += score[k];
            }
            ++res.trialAssignments;
            if (!valid) continue;
            if (!tried || std::tie(total, worst, c.heuristicScore) >
                          std::tie(bestTotal, bestWorst, best->heuristicScore)) {
                best = &c;
                bestWorst = worst;
                bestTotal = total;
                bestEval = worstEval;
                tried = true;
            }
        }

        res.success = true;
        res.timedOut = res.timedOut || expired();
        res.kinds = best->kinds;
        res.supplyCount = best->supplyCount;
        res.heuristicScore = best->heuristicScore;
        res.usedBeamTrial = tried;
        res.trialEvaluation = bestEval;
        res.worstTrialScore = bestWorst;
        res.totalTrialScore = bestTotal;
        return res;
    }
};

inline AgentSelectionResult selectAgentKinds(
    const MatchSetting& setting,
    const AgentSelectorConfig& config = {}) {
    return AgentSelector::select(setting, config);
}

}  // namespace benzen
