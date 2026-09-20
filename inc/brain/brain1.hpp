#pragma once

#include "brain/brain0.hpp"
#include "evaluation.hpp"
#include "game_state_hash.hpp"
#include "plan_validator.hpp"
#include "simulator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

namespace procon37 {

struct BeamSearchConfig {
    std::size_t beamWidth = 128;

    // 同じステップで複数の待機中でないエージェントへ行動を割り当てる際の幅。
    std::size_t jointBeamWidth = 512;
    EvaluationWeights weights{};
};

struct BeamSearchResult {
    bool success = false;
    DailyPlan plan;
    GameState finalState{};
    EvaluationBreakdown evaluation{};
    std::size_t expandedNodes = 0;
};

class Brain1 final {
private:
    using DistanceMatrix = std::vector<std::vector<std::int32_t>>;

    struct BeamNode {
        GameState state{};
        DailyPlan plan;
        EvaluationBreakdown evaluation{};
        double orderingScore = 0.0;
    };

    static bool betterNode(const BeamNode& lhs, const BeamNode& rhs) {
        if (lhs.orderingScore != rhs.orderingScore) {
            return lhs.orderingScore > rhs.orderingScore;
        }
        return lhs.plan < rhs.plan;
    }

    static void sortAndLimit(
        std::vector<BeamNode>& nodes,
        std::size_t limit) {
        std::sort(nodes.begin(), nodes.end(), betterNode);
        if (nodes.size() > limit) {
            nodes.resize(limit);
        }
    }

    static void deduplicateAndLimit(
        std::vector<BeamNode>& nodes,
        std::size_t limit) {
        sortAndLimit(nodes, nodes.size());

        std::unordered_set<GameState, GameStateHash, GameStateEqual> seen;
        seen.reserve(std::min(nodes.size(), limit) * 2 + 1);
        std::vector<BeamNode> unique;
        unique.reserve(std::min(nodes.size(), limit));
        for (BeamNode& node : nodes) {
            if (!seen.insert(node.state).second) {
                continue;
            }
            unique.push_back(std::move(node));
            if (unique.size() == limit) {
                break;
            }
        }
        nodes = std::move(unique);
    }

    // 各 spots[spotIndex] について、全セルからそのスポットまでの最短時間を求める。
    // 辺コストが移動元で決まるので、逆向き緩和時も predecessor のコストを使う。
    static DistanceMatrix buildSpotDistances(
        const MatchSetting& setting,
        const MatchState& matchState) {
        const Field& field = setting.field;
        DistanceMatrix result(
            field.spots.size(),
            std::vector<std::int32_t>(
                field.cellCount(), kImpassableCost));

        using QueueEntry = std::pair<std::int32_t, CellPos>;
        for (std::size_t spotIndex = 0;
             spotIndex < field.spots.size();
             ++spotIndex) {
            const CellPos goal = field.spots[spotIndex].pos;
            if (!field.contains(goal) || !field.at(goal).isPassable()) {
                continue;
            }

            auto& distances = result[spotIndex];
            std::priority_queue<
                QueueEntry,
                std::vector<QueueEntry>,
                std::greater<QueueEntry>>
                queue;
            distances[static_cast<std::size_t>(goal)] = 0;
            queue.emplace(0, goal);

            while (!queue.empty()) {
                const auto [distance, to] = queue.top();
                queue.pop();
                if (distance != distances[static_cast<std::size_t>(to)]) {
                    continue;
                }

                for (const CellPos from : field.neighbors(to)) {
                    if (!field.contains(from) ||
                        !field.at(from).isPassable()) {
                        continue;
                    }
                    const std::int32_t edgeCost =
                        moveStepCost(setting, matchState, from);
                    if (edgeCost >= kImpassableCost ||
                        distance > kImpassableCost - edgeCost) {
                        continue;
                    }

                    const std::int32_t candidate = distance + edgeCost;
                    std::int32_t& current =
                        distances[static_cast<std::size_t>(from)];
                    if (candidate < current) {
                        current = candidate;
                        queue.emplace(candidate, from);
                    }
                }
            }
        }
        return result;
    }

    static const std::vector<std::int32_t>& distancesFrom(
        const MatchSetting& setting,
        const MatchState& matchState,
        CellPos start,
        std::vector<std::vector<std::int32_t>>& cache) {
        static const std::vector<std::int32_t> empty;
        if (!setting.field.contains(start)) {
            return empty;
        }
        auto& distances = cache[static_cast<std::size_t>(start)];
        if (distances.empty()) {
            distances =
                Brain0::dijkstra(setting, matchState, start).distances;
        }
        return distances;
    }

    static CellPos projectedPosition(
        const GameState& state,
        AgentIndex agentIndex) noexcept {
        const PendingAction& pending = state.pendingActions[agentIndex];
        return pending.isMove()
                   ? pending.destination
                   : state.agents[agentIndex].pos;
    }

    static EvaluationContext makeEvaluationContext(
        const GameState& state,
        const MatchSetting& setting,
        const MatchState& matchState,
        ScoreState scoreAtDayStart,
        const DistanceMatrix& spotDistances,
        std::vector<std::vector<std::int32_t>>& distanceCache) {
        EvaluationContext context;
        context.scoreAtDayStart = scoreAtDayStart;

        for (AgentIndex agentIndex = 0;
             agentIndex < state.agentCount;
             ++agentIndex) {
            const Agent& agent = state.agents[agentIndex];
            if (!agent.isPatrol() ||
                !setting.field.contains(agent.pos)) {
                continue;
            }

            const CellPos projected =
                projectedPosition(state, agentIndex);
            const std::int32_t reservedSteps =
                state.pendingActions[agentIndex].active
                    ? state.pendingActions[agentIndex].remainingSteps
                    : 0;

            std::int32_t nearestSpot = kImpassableCost;
            for (std::size_t spotIndex = 0;
                 spotIndex < state.spotCount;
                 ++spotIndex) {
                if (state.remainingStocks[spotIndex] <= 0 ||
                    state.hasAcquired(agentIndex, spotIndex)) {
                    continue;
                }
                const std::int32_t distance =
                    spotDistances[spotIndex]
                                 [static_cast<std::size_t>(projected)];
                if (distance < kImpassableCost &&
                    reservedSteps <= kImpassableCost - distance) {
                    nearestSpot = std::min(
                        nearestSpot, reservedSteps + distance);
                }
            }
            context.nearestValuableSpotSteps[agentIndex] = nearestSpot;

            const auto& distances = distancesFrom(
                setting,
                matchState,
                projected,
                distanceCache);
            std::int32_t nearestSupply = kImpassableCost;
            for (AgentIndex other = 0;
                 other < state.agentCount;
                 ++other) {
                if (!state.agents[other].isSupply()) {
                    continue;
                }
                const CellPos supplyPos =
                    projectedPosition(state, other);
                if (setting.field.contains(supplyPos) &&
                    !distances.empty()) {
                    nearestSupply = std::min(
                        nearestSupply,
                        distances[static_cast<std::size_t>(supplyPos)]);
                }
            }
            context.nearestSupplySteps[agentIndex] = nearestSupply;
        }

        // 同じ現在地・予約移動先へ巡回車が集中する度合い。
        for (AgentIndex lhs = 0; lhs < state.agentCount; ++lhs) {
            if (!state.agents[lhs].isPatrol()) {
                continue;
            }
            const PendingAction& lhsPending = state.pendingActions[lhs];
            const CellPos lhsTarget = lhsPending.isMove()
                                          ? lhsPending.destination
                                          : state.agents[lhs].pos;
            for (AgentIndex rhs = lhs + 1;
                 rhs < state.agentCount;
                 ++rhs) {
                if (!state.agents[rhs].isPatrol()) {
                    continue;
                }
                const PendingAction& rhsPending =
                    state.pendingActions[rhs];
                const CellPos rhsTarget = rhsPending.isMove()
                                              ? rhsPending.destination
                                              : state.agents[rhs].pos;
                if (lhsTarget == rhsTarget) {
                    ++context.concentrationCount;
                }
            }
        }
        return context;
    }

    static EvaluationBreakdown evaluate(
        const GameState& state,
        const MatchSetting& setting,
        const MatchState& matchState,
        ScoreState scoreAtDayStart,
        const DistanceMatrix& spotDistances,
        std::vector<std::vector<std::int32_t>>& distanceCache,
        const EvaluationWeights& weights) {
        const EvaluationContext context = makeEvaluationContext(
            state,
            setting,
            matchState,
            scoreAtDayStart,
            spotDistances,
            distanceCache);
        return evaluateGameState(state, context, weights);
    }

    static double scheduledActionBonus(
        const BeamNode& node,
        AgentIndex agentIndex,
        const MatchSetting& setting,
        const DistanceMatrix& spotDistances,
        const EvaluationWeights& weights) noexcept {
        const Agent& agent = node.state.agents[agentIndex];
        const PendingAction& pending =
            node.state.pendingActions[agentIndex];
        if (!pending.active) {
            return 0.0;
        }

        if (!agent.isPatrol()) {
            return pending.isWait()
                       ? -1.0
                       : -0.01 * pending.remainingSteps;
        }

        const CellPos projected = pending.isMove()
                                      ? pending.destination
                                      : agent.pos;
        std::int32_t nearest = kImpassableCost;
        for (std::size_t spotIndex = 0;
             spotIndex < node.state.spotCount;
             ++spotIndex) {
            if (node.state.remainingStocks[spotIndex] <= 0 ||
                node.state.hasAcquired(agentIndex, spotIndex)) {
                continue;
            }
            nearest = std::min(
                nearest,
                spotDistances[spotIndex]
                             [static_cast<std::size_t>(projected)]);
        }

        double bonus = pending.isWait()
                           ? -1.0
                           : -0.01 * pending.remainingSteps;
        if (nearest < kImpassableCost) {
            bonus += weights.valuableSpotDistance * nearest;
        }

        const Cell& destination = setting.field.at(projected);
        if (destination.hasSpot()) {
            const std::size_t spotIndex =
                static_cast<std::size_t>(destination.spotIndex);
            if (node.state.remainingStocks[spotIndex] > 0 &&
                !node.state.hasAcquired(agentIndex, spotIndex)) {
                const Spot& spot = setting.field.spots[spotIndex];
                const BrandMask bit = brandBit(
                    static_cast<std::size_t>(spot.brandIndex));
                bonus += (node.state.score.matchBrands & bit) == 0
                             ? weights.newBrand * 1.0e-6
                             : weights.udon * 1.0e-2;
            }
        }
        return bonus;
    }

public:
    static BeamSearchResult search(
        const MatchSetting& setting,
        const MatchState& matchState,
        ScoreState previousScore = {},
        const BeamSearchConfig& config = {}) {
        BeamSearchResult result;
        if (config.beamWidth == 0 || config.jointBeamWidth == 0) {
            return result;
        }

        GameState initialState;
        if (!initialState.initialize(
                setting, matchState, previousScore)) {
            return result;
        }

        const DistanceMatrix spotDistances =
            buildSpotDistances(setting, matchState);
        std::vector<std::vector<std::int32_t>> distanceCache(
            setting.field.cellCount());

        BeamNode initial;
        initial.state = initialState;
        initial.plan.resize(initialState.agentCount);
        initial.evaluation = evaluate(
            initial.state,
            setting,
            matchState,
            previousScore,
            spotDistances,
            distanceCache,
            config.weights);
        initial.orderingScore = initial.evaluation.total;

        std::vector<BeamNode> beam;
        beam.push_back(std::move(initial));

        while (!beam.empty() &&
               beam.front().state.step <
                   beam.front().state.dayStepLimit) {
            std::vector<BeamNode> scheduled = std::move(beam);

            // 待機中でない各エージェントへ、合法な6方向移動または
            // 1ステップ待機を割り当てる。
            for (AgentIndex agentIndex = 0;
                 agentIndex < initialState.agentCount;
                 ++agentIndex) {
                std::vector<BeamNode> expanded;
                expanded.reserve(
                    scheduled.size() * static_cast<std::size_t>(7));
                for (const BeamNode& node : scheduled) {
                    if (node.state.pendingActions[agentIndex].active) {
                        expanded.push_back(node);
                        continue;
                    }

                    constexpr std::array<std::int32_t, 7> actions = {
                        0, 1, 2, 3, 4, 5, -1,
                    };
                    for (const std::int32_t action : actions) {
                        BeamNode child = node;
                        const SimulationFailure failure =
                            tryScheduleAction(
                                child.state,
                                setting,
                                matchState,
                                agentIndex,
                                action,
                                child.plan[agentIndex].size());
                        if (failure.error != SimulationError::None) {
                            continue;
                        }
                        child.plan[agentIndex].push_back(action);
                        child.orderingScore =
                            node.orderingScore +
                            scheduledActionBonus(
                                child,
                                agentIndex,
                                setting,
                                spotDistances,
                                config.weights);
                        expanded.push_back(std::move(child));
                        ++result.expandedNodes;
                    }
                }
                sortAndLimit(expanded, config.jointBeamWidth);
                scheduled = std::move(expanded);
                if (scheduled.empty()) {
                    return result;
                }
            }

            std::vector<BeamNode> nextBeam;
            nextBeam.reserve(scheduled.size());
            for (BeamNode& node : scheduled) {
                const SimulationFailure failure =
                    advanceSimulationStep(node.state, setting);
                if (failure.error != SimulationError::None) {
                    continue;
                }
                node.evaluation = evaluate(
                    node.state,
                    setting,
                    matchState,
                    previousScore,
                    spotDistances,
                    distanceCache,
                    config.weights);
                node.orderingScore = node.evaluation.total;
                nextBeam.push_back(std::move(node));
            }
            deduplicateAndLimit(nextBeam, config.beamWidth);
            beam = std::move(nextBeam);
        }

        if (beam.empty()) {
            return result;
        }

        for (BeamNode& node : beam) {
            if (!node.state.finishDay()) {
                node.orderingScore =
                    -std::numeric_limits<double>::infinity();
                continue;
            }
            node.evaluation = evaluate(
                node.state,
                setting,
                matchState,
                previousScore,
                spotDistances,
                distanceCache,
                config.weights);
            node.orderingScore = node.evaluation.total;
        }
        sortAndLimit(beam, 1);
        if (beam.empty() ||
            !std::isfinite(beam.front().orderingScore)) {
            return result;
        }

        const PlanValidationResult validation = validateDailyPlan(
            setting,
            matchState,
            beam.front().plan,
            previousScore);
        if (!validation.valid) {
            return result;
        }

        result.success = true;
        result.plan = std::move(beam.front().plan);
        result.finalState = beam.front().state;
        result.evaluation = beam.front().evaluation;
        return result;
    }
};

inline BeamSearchResult brain1(
    const MatchSetting& setting,
    const MatchState& matchState,
    ScoreState previousScore = {},
    const BeamSearchConfig& config = {}) {
    return Brain1::search(setting, matchState, previousScore, config);
}

}  // namespace procon37
