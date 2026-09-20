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
        const MatchSetting& setting,
        const AgentSelectorConfig& config) {
        const std::size_t agentCount = setting.agentCount();
        if (agentCount == 0 || agentCount > kMaxAgents) {
            throw std::invalid_argument(
                "agent count must be in [1, kMaxAgents]");
        }
        if (setting.daySteps.empty() ||
            !setting.hasConsistentDaySettings() ||
            setting.fuelLimit <= 0 ||
            setting.field.cells.size() != setting.field.cellCount()) {
            throw std::invalid_argument("match setting is incomplete");
        }
        for (const CellPos start : setting.agentStarts) {
            if (!setting.field.contains(start) ||
                !setting.field.at(start).isPassable()) {
                throw std::invalid_argument(
                    "every agent start must be passable");
            }
        }

        const std::int32_t maximum =
            config.maxSupplyCount < 0
                ? static_cast<std::int32_t>(agentCount) - 1
                : config.maxSupplyCount;
        if (config.minSupplyCount < 0 ||
            config.minSupplyCount > maximum ||
            maximum >= static_cast<std::int32_t>(agentCount)) {
            throw std::invalid_argument(
                "supply count range must leave at least one patrol agent");
        }
        if (config.trialCandidateLimit > 0 &&
            (config.trialBeamWidth == 0 ||
             config.trialJointBeamWidth == 0)) {
            throw std::invalid_argument(
                "beam widths must be positive when trials are enabled");
        }
    }

    static MatchState makeState(
        const MatchSetting& setting,
        const std::vector<AgentKind>& kinds) {
        MatchState state;
        state.day = 0;
        state.trafficByPos.assign(
            setting.field.cellCount(), TrafficStatus::Smooth);
        state.agents.reserve(kinds.size());
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            state.agents.push_back(Agent{
                i,
                kinds[i],
                setting.agentStarts[i],
                setting.fuelLimit,
            });
        }
        return state;
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
        const MatchSetting& setting,
        const AgentSelectorConfig& config = {}) {
        validate(setting, config);

        const std::size_t agentCount = setting.agentCount();
        const std::int32_t maximumSupply =
            config.maxSupplyCount < 0
                ? static_cast<std::int32_t>(agentCount) - 1
                : config.maxSupplyCount;
        const std::int32_t daySteps = setting.daySteps.front();
        const std::uint32_t assignmentCount =
            std::uint32_t{1} << static_cast<std::uint32_t>(agentCount);

        MatchState allPatrolState;
        allPatrolState.day = 0;
        allPatrolState.trafficByPos.assign(
            setting.field.cellCount(), TrafficStatus::Smooth);
        std::vector<DijkstraTable> distances;
        distances.reserve(agentCount);
        for (const CellPos start : setting.agentStarts) {
            distances.push_back(
                Brain0::dijkstra(setting, allPatrolState, start));
        }

        std::vector<Candidate> candidates;
        for (std::uint32_t mask = 0; mask < assignmentCount; ++mask) {
            const std::int32_t supplyCount = countBits(mask);
            if (supplyCount < config.minSupplyCount ||
                supplyCount > maximumSupply) {
                continue;
            }

            Candidate candidate;
            candidate.supplyCount = supplyCount;
            candidate.supplyMask = mask;
            candidate.kinds.resize(agentCount, AgentKind::Patrol);
            for (std::size_t i = 0; i < agentCount; ++i) {
                if ((mask & (std::uint32_t{1} << i)) != 0) {
                    candidate.kinds[i] = AgentKind::Supply;
                }
            }

            std::vector<bool> reachableBrands(
                setting.field.brands.size(), false);
            std::int32_t reachableSpots = 0;
            std::int32_t reachableStock = 0;
            std::int64_t distanceSum = 0;
            for (const Spot& spot : setting.field.spots) {
                std::int32_t nearest = kImpassableCost;
                for (std::size_t i = 0; i < agentCount; ++i) {
                    if (candidate.kinds[i] != AgentKind::Patrol) {
                        continue;
                    }
                    nearest = std::min(
                        nearest,
                        distanceTo(distances[i], spot.pos));
                }
                if (nearest > daySteps || nearest >= kImpassableCost) {
                    continue;
                }
                ++reachableSpots;
                reachableStock += spot.maxStock;
                distanceSum += nearest;
                if (spot.brandIndex >= 0 &&
                    static_cast<std::size_t>(spot.brandIndex) <
                        reachableBrands.size()) {
                    reachableBrands[
                        static_cast<std::size_t>(spot.brandIndex)] = true;
                }
            }

            const std::int32_t brandCount = static_cast<std::int32_t>(
                std::count(
                    reachableBrands.begin(),
                    reachableBrands.end(),
                    true));

            // 同じ補給車台数なら、巡回車と主要スポットに近い開始位置を
            // 補給車へ割り当てる。燃料が少ない問題ほど影響を強める。
            std::int64_t supplyDistance = 0;
            if (supplyCount > 0) {
                for (std::size_t patrol = 0;
                     patrol < agentCount;
                     ++patrol) {
                    if (candidate.kinds[patrol] != AgentKind::Patrol) {
                        continue;
                    }
                    std::int32_t nearest = kImpassableCost;
                    for (std::size_t supply = 0;
                         supply < agentCount;
                         ++supply) {
                        if (candidate.kinds[supply] != AgentKind::Supply) {
                            continue;
                        }
                        nearest = std::min(
                            nearest,
                            distanceTo(
                                distances[supply],
                                setting.agentStarts[patrol]));
                    }
                    if (nearest < kImpassableCost) {
                        supplyDistance += nearest;
                    }
                }
            }

            const double maximumFuelDemand =
                3.0 * static_cast<double>(std::max(1, daySteps));
            const double fuelPressure = std::max(
                0.0,
                (maximumFuelDemand - setting.fuelLimit) /
                    maximumFuelDemand);
            candidate.heuristicScore =
                1.0e9 * brandCount +
                1.0e6 * reachableSpots +
                1.0e4 * reachableStock -
                100.0 * static_cast<double>(distanceSum) -
                10.0 * fuelPressure *
                    static_cast<double>(supplyDistance);
            candidates.push_back(std::move(candidate));
        }

        std::sort(candidates.begin(), candidates.end(), betterHeuristic);
        AgentSelectionResult result;
        result.evaluatedAssignments = candidates.size();
        if (candidates.empty()) {
            return result;
        }

        std::vector<std::size_t> trialIndices;
        if (config.trialCandidateLimit > 0) {
            // 補給車台数ごとの最良配置を優先し、台数の違いを必ず比較する。
            for (std::int32_t supplies = config.minSupplyCount;
                 supplies <= maximumSupply &&
                 trialIndices.size() < config.trialCandidateLimit;
                 ++supplies) {
                const auto found = std::find_if(
                    candidates.begin(),
                    candidates.end(),
                    [supplies](const Candidate& candidate) {
                        return candidate.supplyCount == supplies;
                    });
                if (found != candidates.end()) {
                    trialIndices.push_back(static_cast<std::size_t>(
                        found - candidates.begin()));
                }
            }
            for (std::size_t i = 0;
                 i < candidates.size() &&
                 trialIndices.size() < config.trialCandidateLimit;
                 ++i) {
                if (std::find(trialIndices.begin(), trialIndices.end(), i) ==
                    trialIndices.end()) {
                    trialIndices.push_back(i);
                }
            }
        }

        const Candidate* selected = &candidates.front();
        EvaluationBreakdown selectedEvaluation{};
        bool hasSuccessfulTrial = false;
        BeamSearchConfig beamConfig;
        beamConfig.beamWidth = config.trialBeamWidth;
        beamConfig.jointBeamWidth = config.trialJointBeamWidth;
        beamConfig.weights = config.trialWeights;
        for (const std::size_t index : trialIndices) {
            const Candidate& candidate = candidates[index];
            const MatchState state = makeState(setting, candidate.kinds);
            const BeamSearchResult trial =
                Brain1::search(setting, state, {}, beamConfig);
            ++result.trialAssignments;
            if (!trial.success) {
                continue;
            }
            if (!hasSuccessfulTrial ||
                betterTrial(
                    trial.evaluation,
                    candidate.heuristicScore,
                    selectedEvaluation,
                    selected->heuristicScore)) {
                selected = &candidate;
                selectedEvaluation = trial.evaluation;
                hasSuccessfulTrial = true;
            }
        }

        result.success = true;
        result.kinds = selected->kinds;
        result.supplyCount = selected->supplyCount;
        result.heuristicScore = selected->heuristicScore;
        result.usedBeamTrial = hasSuccessfulTrial;
        result.trialEvaluation = selectedEvaluation;
        return result;
    }
};

inline AgentSelectionResult selectAgentKinds(
    const MatchSetting& setting,
    const AgentSelectorConfig& config = {}) {
    return AgentSelector::select(setting, config);
}

}  // namespace procon37
