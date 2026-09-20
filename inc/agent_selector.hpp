#pragma once

#include "brain/brain0.hpp"
#include "brain/brain1.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace procon37 {

struct AgentSelectorConfig {
    // -1なら「全車が補給車」だけを除いた最大台数まで調べる。
    std::int32_t minSupplyCount = 0;
    std::int32_t maxSupplyCount = -1;

    // 距離評価後にbrain1で試走する候補数。0なら距離評価だけを使う。
    std::size_t trialCandidateLimit = 8;
    std::size_t trialBeamWidth = 32;
    std::size_t trialJointBeamWidth = 128;
    EvaluationWeights trialWeights{};
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
};

class AgentSelector final {
private:
    struct Candidate {
        std::vector<AgentKind> kinds;
        std::int32_t supplyCount = 0;
        double heuristicScore = 0.0;
        std::uint32_t supplyMask = 0;
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
    }

    static MatchState makeState(
        const MatchSetting& s,
        const std::vector<AgentKind>& kinds) {
        MatchState st;
        st.day = 0;
        st.trafficByPos.assign(s.field.cellCount(), TrafficStatus::Smooth);
        st.agents.reserve(kinds.size());
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            st.agents.push_back(
                Agent{i, kinds[i], s.agentStarts[i], s.fuelLimit});
        }
        return st;
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

    static bool betterTrial(
        const EvaluationBreakdown& lhs,
        double lhsHeuristic,
        const EvaluationBreakdown& rhs,
        double rhsHeuristic) noexcept {
        return std::tie(
                   lhs.newBrandCount,
                   lhs.dailyBrandCount,
                   lhs.udonCount,
                   lhs.remainingFuel,
                   lhsHeuristic) >
               std::tie(
                   rhs.newBrandCount,
                   rhs.dailyBrandCount,
                   rhs.udonCount,
                   rhs.remainingFuel,
                   rhsHeuristic);
    }

public:
    static AgentSelectionResult select(
        const MatchSetting& s,
        const AgentSelectorConfig& cfg = {}) {
        validate(s, cfg);

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
            dist.push_back(Brain0::dijkstra(s, base, start));
        }

        std::vector<Candidate> cs;
        for (std::uint32_t mask = 0; mask < masks; ++mask) {
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

            const double maxFuel =
                3.0 * static_cast<double>(std::max(1, steps));
            const double fuelPressure = std::max(
                0.0,
                (maxFuel - s.fuelLimit) / maxFuel);
            c.heuristicScore =
                1.0e9 * brandN + 1.0e6 * spots + 1.0e4 * stock -
                100.0 * static_cast<double>(distSum) -
                10.0 * fuelPressure * static_cast<double>(supplyDist);
            cs.push_back(std::move(c));
        }

        std::sort(cs.begin(), cs.end(), betterHeuristic);
        AgentSelectionResult res;
        res.evaluatedAssignments = cs.size();
        if (cs.empty()) {
            return res;
        }

        std::vector<std::size_t> trials;
        if (cfg.trialCandidateLimit > 0) {
            // 補給車台数ごとの最良配置を優先し、台数の違いを必ず比較する。
            for (std::int32_t count = cfg.minSupplyCount;
                 count <= maxSupply &&
                 trials.size() < cfg.trialCandidateLimit;
                 ++count) {
                const auto found = std::find_if(
                    cs.begin(),
                    cs.end(),
                    [count](const Candidate& c) {
                        return c.supplyCount == count;
                    });
                if (found != cs.end()) {
                    trials.push_back(static_cast<std::size_t>(
                        found - cs.begin()));
                }
            }
            for (std::size_t i = 0;
                 i < cs.size() && trials.size() < cfg.trialCandidateLimit;
                 ++i) {
                if (std::find(trials.begin(), trials.end(), i) ==
                    trials.end()) {
                    trials.push_back(i);
                }
            }
        }

        const Candidate* best = &cs.front();
        EvaluationBreakdown bestEval{};
        bool tried = false;
        BeamSearchConfig beam;
        beam.beamWidth = cfg.trialBeamWidth;
        beam.jointBeamWidth = cfg.trialJointBeamWidth;
        beam.weights = cfg.trialWeights;
        for (const std::size_t i : trials) {
            const Candidate& c = cs[i];
            const BeamSearchResult trial =
                Brain1::search(s, makeState(s, c.kinds), {}, beam);
            ++res.trialAssignments;
            if (!trial.success) {
                continue;
            }
            if (!tried ||
                betterTrial(
                    trial.evaluation,
                    c.heuristicScore,
                    bestEval,
                    best->heuristicScore)) {
                best = &c;
                bestEval = trial.evaluation;
                tried = true;
            }
        }

        res.success = true;
        res.kinds = best->kinds;
        res.supplyCount = best->supplyCount;
        res.heuristicScore = best->heuristicScore;
        res.usedBeamTrial = tried;
        res.trialEvaluation = bestEval;
        return res;
    }
};

inline AgentSelectionResult selectAgentKinds(
    const MatchSetting& setting,
    const AgentSelectorConfig& config = {}) {
    return AgentSelector::select(setting, config);
}

}  // namespace procon37
