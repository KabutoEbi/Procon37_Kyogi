#pragma once

#include "brain/brain0.hpp"
#include "evaluation.hpp"
#include "game_state_hash.hpp"
#include "plan_validator.hpp"
#include "simulator.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace procon37 {

struct BeamSearchConfig {
    std::size_t beamWidth = 128;

    // 同じステップで複数の待機中でないエージェントへ行動を割り当てる際の幅。
    std::size_t jointBeamWidth = 512;
    EvaluationWeights weights{};

    // 0なら明示的な相対制限なし。endsAtとの短い方を採用する。
    std::int64_t timeLimitMs = 0;
    std::int64_t safetyMarginMs = 1000;
    std::size_t timeCheckInterval = 256;
    bool useStateDeadline = true;

    // 0ならハードウェア並列数（最大8）を使う。1なら並列処理を無効化する。
    std::size_t threadCount = 0;
    std::size_t parallelThreshold = 64;

    // 大きい盤面・多数エージェント・締切直前では幅を自動的に絞る。
    bool adaptiveBeamWidth = true;
    std::size_t minimumBeamWidth = 16;
    std::size_t minimumJointBeamWidth = 64;

    // 有効な途中解を再シミュレーションして保存する間隔。
    std::size_t incumbentInterval = 4;
};

enum class SearchStopReason : std::uint8_t {
    Completed,
    TimeLimit,
    NoValidPlan,
};

struct BeamSearchResult {
    bool success = false;
    DailyPlan plan;
    GameState finalState{};
    EvaluationBreakdown evaluation{};
    std::size_t expandedNodes = 0;
    SearchStopReason stopReason = SearchStopReason::NoValidPlan;
    std::int64_t elapsedMs = 0;
};

class Brain1 final {
private:
    using DistanceMatrix = std::vector<std::vector<std::int32_t>>;

    struct SpotDistanceTables {
        DistanceMatrix steps;
        DistanceMatrix fuel;
    };

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
        if (limit == 0) {
            nodes.clear();
            return;
        }
        if (nodes.size() > limit) {
            std::nth_element(
                nodes.begin(),
                nodes.begin() + static_cast<std::ptrdiff_t>(limit),
                nodes.end(),
                betterNode);
            nodes.resize(limit);
        }
        std::sort(nodes.begin(), nodes.end(), betterNode);
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

    // 各スポットへの最短時間と巡回車の最小必要燃料を求める。
    // スポットごとの行は独立しているため、複数スレッドで構築できる。
    static SpotDistanceTables buildSpotDistances(
        const MatchSetting& setting,
        const MatchState& matchState,
        const std::function<bool()>& stop,
        std::size_t threadCount,
        bool& stopped) {
        const Field& field = setting.field;
        SpotDistanceTables result;
        result.steps.assign(
            field.spots.size(),
            std::vector<std::int32_t>(
                field.cellCount(), kImpassableCost));
        result.fuel = result.steps;

        std::atomic<std::size_t> nextSpot{0};
        std::atomic<bool> aborted{false};

        using QueueEntry = std::pair<std::int32_t, CellPos>;
        auto buildRow = [&](std::size_t spotIndex, bool fuel) {
            const CellPos goal = field.spots[spotIndex].pos;
            if (!field.contains(goal) || !field.at(goal).isPassable()) {
                return;
            }
            auto& distances = fuel ? result.fuel[spotIndex]
                                   : result.steps[spotIndex];
            std::priority_queue<
                QueueEntry,
                std::vector<QueueEntry>,
                std::greater<QueueEntry>> queue;
            distances[static_cast<std::size_t>(goal)] = 0;
            queue.emplace(0, goal);
            while (!queue.empty() && !aborted.load(std::memory_order_relaxed)) {
                if (stop()) {
                    aborted.store(true, std::memory_order_relaxed);
                    return;
                }
                const auto [distance, to] = queue.top();
                queue.pop();
                if (distance != distances[static_cast<std::size_t>(to)]) {
                    continue;
                }
                for (const CellPos from : field.neighbors(to)) {
                    if (!field.contains(from) || !field.at(from).isPassable()) {
                        continue;
                    }
                    const std::int32_t edgeCost = fuel
                        ? moveFuelCost(setting, AgentKind::Patrol, from)
                        : moveStepCost(setting, matchState, from);
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
        };

        auto worker = [&]() {
            while (!aborted.load(std::memory_order_relaxed)) {
                const std::size_t task =
                    nextSpot.fetch_add(1, std::memory_order_relaxed);
                if (task >= field.spots.size() * 2) {
                    return;
                }
                buildRow(task / 2, task % 2 != 0);
            }
        };

        const std::size_t workers = std::max<std::size_t>(
            1,
            std::min(threadCount, field.spots.size() * 2));
        std::vector<std::thread> threads;
        threads.reserve(workers > 0 ? workers - 1 : 0);
        for (std::size_t i = 1; i < workers; ++i) {
            threads.emplace_back(worker);
        }
        worker();
        for (std::thread& thread : threads) {
            thread.join();
        }
        stopped = aborted.load(std::memory_order_relaxed);
        return result;
    }

    static CellPos projectedPosition(
        const GameState& state,
        AgentIndex agentIndex) noexcept {
        const PendingAction& pending = state.pendingActions[agentIndex];
        return pending.isMove()
                   ? pending.destination
                   : state.agents[agentIndex].pos;
    }

    static bool canScheduleAction(
        const GameState& state,
        const MatchSetting& setting,
        const MatchState& matchState,
        AgentIndex agentIndex,
        std::int32_t action) noexcept {
        if (agentIndex >= state.agentCount ||
            state.pendingActions[agentIndex].active ||
            state.remainingSteps() <= 0) {
            return false;
        }
        if (isWaitAction(action)) {
            return action != std::numeric_limits<std::int32_t>::min() &&
                   -action <= state.remainingSteps();
        }
        if (!isMoveAction(action)) {
            return false;
        }
        const Agent& agent = state.agents[agentIndex];
        if (!setting.field.contains(agent.pos)) {
            return false;
        }
        const CellPos destination = setting.field.neighbor(
            agent.pos, static_cast<Direction>(action));
        if (!setting.field.contains(destination) ||
            !setting.field.at(destination).isPassable()) {
            return false;
        }
        const std::int32_t steps =
            moveStepCost(setting, matchState, agent.pos);
        const std::int32_t fuel =
            moveFuelCost(setting, agent.kind, agent.pos);
        return steps <= state.remainingSteps() &&
               (!agent.isPatrol() || fuel <= agent.fuel);
    }

    static std::int32_t projectedDelay(
        const GameState& state,
        AgentIndex agentIndex) noexcept {
        const PendingAction& pending = state.pendingActions[agentIndex];
        return pending.active ? pending.remainingSteps : 0;
    }

    // 障害物を無視した六角距離。補給合流の軽量な下界評価に使う。
    static std::int32_t hexDistance(
        const Field& field,
        CellPos lhs,
        CellPos rhs) noexcept {
        const Coord a = field.toCoord(lhs);
        const Coord b = field.toCoord(rhs);
        if (!field.contains(a) || !field.contains(b)) {
            return kImpassableCost;
        }
        auto cube = [](Coord p) {
            const std::int32_t q = p.x - (p.y + (p.y & 1)) / 2;
            const std::int32_t r = p.y;
            return std::array<std::int32_t, 3>{q, r, -q - r};
        };
        const auto ca = cube(a);
        const auto cb = cube(b);
        return std::max({
            std::abs(ca[0] - cb[0]),
            std::abs(ca[1] - cb[1]),
            std::abs(ca[2] - cb[2]),
        });
    }

    static EvaluationContext makeEvaluationContext(
        const GameState& state,
        const MatchSetting& setting,
        ScoreState scoreAtDayStart,
        const SpotDistanceTables& spotDistances) {
        EvaluationContext context;
        context.scoreAtDayStart = scoreAtDayStart;
        std::array<std::int32_t, kMaxAgents> targets{};
        targets.fill(-1);

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
                projectedDelay(state, agentIndex);
            const std::int32_t availableSteps =
                std::max(0, state.remainingSteps() - reservedSteps);

            std::int32_t nearestSpot = kImpassableCost;
            std::int32_t nearestNew = kImpassableCost;
            std::int32_t nearestDaily = kImpassableCost;
            std::int32_t targetClass = 3;
            std::int32_t targetDistance = kImpassableCost;
            bool hasCandidate = false;
            bool hasTimeReachableCandidate = false;
            for (std::size_t spotIndex = 0;
                 spotIndex < state.spotCount;
                 ++spotIndex) {
                if (state.remainingStocks[spotIndex] <= 0 ||
                    state.hasAcquired(agentIndex, spotIndex)) {
                    continue;
                }
                hasCandidate = true;
                const std::size_t pos = static_cast<std::size_t>(projected);
                const std::int32_t distance =
                    spotDistances.steps[spotIndex][pos];
                const std::int32_t fuel =
                    spotDistances.fuel[spotIndex][pos];
                if (distance >= kImpassableCost ||
                    distance > availableSteps) {
                    continue;
                }
                hasTimeReachableCandidate = true;
                if (fuel > agent.fuel) {
                    continue;
                }
                const Spot& spot = setting.field.spots[spotIndex];
                const BrandMask bit = brandBit(
                    static_cast<std::size_t>(spot.brandIndex));
                const bool newMatch = (state.score.matchBrands & bit) == 0;
                const bool newDay = (state.dayBrands & bit) == 0;
                const std::int32_t kind = newMatch ? 0 : (newDay ? 1 : 2);
                const std::int32_t total = reservedSteps + distance;
                nearestSpot = std::min(nearestSpot, total);
                if (newMatch) {
                    nearestNew = std::min(nearestNew, total);
                }
                if (newDay) {
                    nearestDaily = std::min(nearestDaily, total);
                }
                if (kind < targetClass ||
                    (kind == targetClass && distance < targetDistance) ||
                    (kind == targetClass && distance == targetDistance &&
                     static_cast<std::int32_t>(spotIndex) <
                         targets[agentIndex])) {
                    targetClass = kind;
                    targetDistance = distance;
                    targets[agentIndex] =
                        static_cast<std::int32_t>(spotIndex);
                }
            }
            context.nearestValuableSpotSteps[agentIndex] = nearestSpot;
            context.nearestNewBrandSpotSteps[agentIndex] = nearestNew;
            context.nearestDailyBrandSpotSteps[agentIndex] = nearestDaily;
            if (targetDistance < kImpassableCost) {
                context.targetSlack += availableSteps - targetDistance;
            } else if (hasCandidate) {
                ++context.unreachableTargetCount;
                if (hasTimeReachableCandidate) {
                    context.fuelRisk += std::max(
                        1, (setting.fuelLimit + 1) / 2 - agent.fuel);
                }
            }

            const std::int32_t fuelThreshold = std::max(
                1, (setting.fuelLimit + 1) / 2);
            const std::int32_t fuelNeed =
                std::max(0, fuelThreshold - agent.fuel);
            std::int32_t nearestSupply = kImpassableCost;
            std::int32_t bestMismatch = kImpassableCost;
            for (AgentIndex other = 0;
                 other < state.agentCount;
                 ++other) {
                if (!state.agents[other].isSupply()) {
                    continue;
                }
                const CellPos supplyPos =
                    projectedPosition(state, other);
                const std::int32_t distance = hexDistance(
                    setting.field, projected, supplyPos);
                if (distance < kImpassableCost) {
                    nearestSupply = std::min(nearestSupply, distance);
                    const std::int32_t supplyDelay =
                        projectedDelay(state, other);
                    bestMismatch = std::min(
                        bestMismatch,
                        std::min(
                            std::abs(
                                reservedSteps + distance - supplyDelay),
                            std::abs(
                                supplyDelay + distance - reservedSteps)));
                }
            }
            if (fuelNeed > 0 && nearestSupply < kImpassableCost) {
                context.nearestSupplySteps[agentIndex] = nearestSupply;
                context.rendezvousMismatch += bestMismatch;
                context.fuelRisk += fuelNeed;
            }
        }

        // 同一スポットの残存在庫を超えて巡回車が集中する分を罰する。
        for (std::size_t spotIndex = 0;
             spotIndex < state.spotCount;
             ++spotIndex) {
            std::int32_t assigned = 0;
            for (AgentIndex agentIndex = 0;
                 agentIndex < state.agentCount;
                 ++agentIndex) {
                assigned += targets[agentIndex] ==
                            static_cast<std::int32_t>(spotIndex);
            }
            context.concentrationCount += std::max(
                0, assigned - state.remainingStocks[spotIndex]);
        }
        return context;
    }

    static EvaluationBreakdown evaluate(
        const GameState& state,
        const MatchSetting& setting,
        ScoreState scoreAtDayStart,
        const SpotDistanceTables& spotDistances,
        const EvaluationWeights& weights) {
        const EvaluationContext context = makeEvaluationContext(
            state,
            setting,
            scoreAtDayStart,
            spotDistances);
        return evaluateGameState(state, context, weights);
    }

    static double scheduledActionBonus(
        const BeamNode& node,
        AgentIndex agentIndex,
        const MatchSetting& setting,
        const SpotDistanceTables& spotDistances,
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
        std::int32_t priority = 3;
        for (std::size_t spotIndex = 0;
             spotIndex < node.state.spotCount;
             ++spotIndex) {
            if (node.state.remainingStocks[spotIndex] <= 0 ||
                node.state.hasAcquired(agentIndex, spotIndex)) {
                continue;
            }
            const std::size_t pos = static_cast<std::size_t>(projected);
            const std::int32_t distance =
                spotDistances.steps[spotIndex][pos];
            const std::int32_t fuel =
                spotDistances.fuel[spotIndex][pos];
            if (fuel > agent.fuel) {
                continue;
            }
            const Spot& spot = setting.field.spots[spotIndex];
            const BrandMask bit = brandBit(
                static_cast<std::size_t>(spot.brandIndex));
            const std::int32_t kind =
                (node.state.score.matchBrands & bit) == 0
                    ? 0
                    : ((node.state.dayBrands & bit) == 0 ? 1 : 2);
            if (kind < priority ||
                (kind == priority && distance < nearest)) {
                priority = kind;
                nearest = distance;
            }
        }

        double bonus = pending.isWait()
                           ? -1.0
                           : -0.01 * pending.remainingSteps;
        if (nearest < kImpassableCost) {
            bonus += weights.valuableSpotDistance * nearest;
            if (priority == 0) {
                bonus += weights.newBrandSpotDistance * nearest;
            } else if (priority == 1) {
                bonus += weights.dailyBrandSpotDistance * nearest;
            }
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
        const MatchSetting& s,
        const MatchState& ms,
        ScoreState prev = {},
        const BeamSearchConfig& cfg = {}) {
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        BeamSearchResult res;
        auto finish = [&](SearchStopReason reason) {
            if (res.success) {
                res.plan = compressWaitActions(res.plan);
            }
            res.stopReason = reason;
            res.elapsedMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(Clock::now() - started).count();
            return res;
        };

        if (cfg.beamWidth == 0 || cfg.jointBeamWidth == 0 ||
            cfg.timeLimitMs < 0 || cfg.safetyMarginMs < 0 ||
            cfg.incumbentInterval == 0) {
            return finish(SearchStopReason::NoValidPlan);
        }

        GameState init;
        if (!init.initialize(s, ms, prev)) {
            return finish(SearchStopReason::NoValidPlan);
        }

        std::int64_t budgetMs = cfg.timeLimitMs > 0
                                    ? cfg.timeLimitMs
                                    : std::numeric_limits<std::int64_t>::max();
        if (cfg.useStateDeadline && ms.endsAt > 0) {
            const auto end = std::chrono::system_clock::time_point{
                std::chrono::seconds(ms.endsAt)};
            const auto left = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                end - std::chrono::system_clock::now()).count();
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
        const std::size_t hardwareThreads = std::min<std::size_t>(
            8,
            std::max(1U, std::thread::hardware_concurrency()));
        const std::size_t requestedThreads =
            cfg.threadCount == 0 ? hardwareThreads : cfg.threadCount;
        const std::size_t threadCount = std::clamp<std::size_t>(
            requestedThreads, 1, hardwareThreads);

        std::atomic<std::size_t> work{0};
        const std::size_t checkEvery =
            std::max<std::size_t>(1, cfg.timeCheckInterval);
        auto expired = [&](bool force = false) {
            if (!limited) {
                return false;
            }
            const std::size_t count =
                work.fetch_add(1, std::memory_order_relaxed) + 1;
            return (force || count % checkEvery == 0) &&
                   Clock::now() >= deadline;
        };

        auto adaptiveLimit = [&](std::size_t configured,
                                 std::size_t minimum) {
            if (!cfg.adaptiveBeamWidth || configured <= minimum) {
                return configured;
            }
            double scale = std::min(
                1.0,
                256.0 / static_cast<double>(
                            std::max<std::size_t>(256, s.field.cellCount())));
            scale *= std::min(
                1.0,
                4.0 / static_cast<double>(
                          std::max<std::size_t>(4, init.agentCount)));
            if (limited && budgetMs > 0) {
                const auto left = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - Clock::now()).count();
                const double ratio = static_cast<double>(
                    std::max<std::int64_t>(0, left)) / budgetMs;
                if (ratio < 0.15) {
                    scale *= 0.35;
                } else if (ratio < 0.35) {
                    scale *= 0.60;
                }
            }
            const std::size_t scaled = static_cast<std::size_t>(
                std::ceil(configured * scale));
            return std::min(configured, std::max(minimum, scaled));
        };

        const DailyPlan wait(
            init.agentCount, AgentPlan{-init.dayStepLimit});
        const SimulationResult fallback = simulateDay(s, ms, wait, prev);
        bool hasBest = fallback.valid;
        if (hasBest) {
            EvaluationContext ctx;
            ctx.scoreAtDayStart = prev;
            res.success = true;
            res.plan = wait;
            res.finalState = fallback.state;
            res.evaluation = evaluateGameState(
                fallback.state, ctx, cfg.weights);
        }
        if (expired(true)) {
            return finish(
                hasBest ? SearchStopReason::TimeLimit
                        : SearchStopReason::NoValidPlan);
        }

        bool prepStopped = false;
        const SpotDistanceTables spotDist = buildSpotDistances(
            s,
            ms,
            [&]() { return expired(); },
            threadCount,
            prepStopped);
        if (prepStopped) {
            return finish(
                hasBest ? SearchStopReason::TimeLimit
                        : SearchStopReason::NoValidPlan);
        }
        BeamNode root;
        root.state = init;
        root.plan.resize(init.agentCount);
        root.evaluation =
            evaluate(root.state, s, prev, spotDist, cfg.weights);
        root.orderingScore = root.evaluation.total;

        auto consider = [&](const BeamNode& node) {
            DailyPlan plan = node.plan;
            for (AgentIndex i = 0; i < node.state.agentCount; ++i) {
                const PendingAction& pending = node.state.pendingActions[i];
                const std::int32_t reserved =
                    pending.active ? pending.remainingSteps : 0;
                const std::int32_t wait =
                    node.state.remainingSteps() - reserved;
                if (wait > 0) {
                    appendPlanAction(plan[i], -wait);
                }
            }

            const SimulationResult sim = simulateDay(s, ms, plan, prev);
            if (!sim.valid) {
                return;
            }
            const EvaluationBreakdown eval = evaluate(
                sim.state, s, prev, spotDist, cfg.weights);
            if (hasBest &&
                (eval.total < res.evaluation.total ||
                 (eval.total == res.evaluation.total &&
                  !(plan < res.plan)))) {
                return;
            }
            hasBest = true;
            res.success = true;
            res.plan = std::move(plan);
            res.finalState = sim.state;
            res.evaluation = eval;
        };
        if (hasBest) {
            res.evaluation = evaluate(
                res.finalState, s, prev, spotDist, cfg.weights);
        }
        consider(root);
        if (expired(true)) {
            return finish(
                hasBest ? SearchStopReason::TimeLimit
                        : SearchStopReason::NoValidPlan);
        }

        std::vector<BeamNode> beam{std::move(root)};

        while (!beam.empty() &&
               beam.front().state.step <
                   beam.front().state.dayStepLimit) {
            std::vector<BeamNode> ready = std::move(beam);

            // 待機中でない各エージェントへ、合法な6方向移動または
            // 1ステップ待機を割り当てる。
            for (AgentIndex a = 0; a < init.agentCount; ++a) {
                std::vector<BeamNode> next;
                next.reserve(ready.size() * 7);
                for (const BeamNode& node : ready) {
                    if (node.state.pendingActions[a].active) {
                        next.push_back(node);
                        continue;
                    }

                    constexpr std::array<std::int32_t, 7> actions = {
                        0, 1, 2, 3, 4, 5, -1,
                    };
                    for (const std::int32_t action : actions) {
                        if (expired()) {
                            return finish(
                                hasBest ? SearchStopReason::TimeLimit
                                        : SearchStopReason::NoValidPlan);
                        }
                        if (!canScheduleAction(
                                node.state, s, ms, a, action)) {
                            continue;
                        }
                        BeamNode child = node;
                        const auto err = tryScheduleAction(
                            child.state,
                            s,
                            ms,
                            a,
                            action,
                            child.plan[a].size());
                        if (err.error != SimulationError::None) {
                            continue;
                        }
                        appendPlanAction(child.plan[a], action);
                        child.orderingScore =
                            node.orderingScore +
                            scheduledActionBonus(
                                child, a, s, spotDist, cfg.weights);
                        next.push_back(std::move(child));
                        ++res.expandedNodes;
                    }
                }
                sortAndLimit(
                    next,
                    adaptiveLimit(
                        cfg.jointBeamWidth,
                        cfg.minimumJointBeamWidth));
                ready = std::move(next);
                if (ready.empty()) {
                    return finish(
                        hasBest ? SearchStopReason::Completed
                                : SearchStopReason::NoValidPlan);
                }
            }

            std::vector<BeamNode> next;
            next.reserve(ready.size());
            const bool parallel = threadCount > 1 &&
                                  ready.size() >= cfg.parallelThreshold;
            std::atomic<bool> evaluationTimedOut{false};
            auto evaluateRange = [&](std::size_t begin,
                                     std::size_t end,
                                     std::vector<BeamNode>& output) {
                output.reserve(end - begin);
                for (std::size_t i = begin; i < end; ++i) {
                    if (expired()) {
                        evaluationTimedOut.store(
                            true, std::memory_order_relaxed);
                        return;
                    }
                    BeamNode& node = ready[i];
                    const auto err = advanceSimulationStep(node.state, s);
                    if (err.error != SimulationError::None) {
                        continue;
                    }
                    node.evaluation = evaluate(
                        node.state, s, prev, spotDist, cfg.weights);
                    node.orderingScore = node.evaluation.total;
                    output.push_back(std::move(node));
                }
            };

            if (parallel) {
                const std::size_t workers = std::min(
                    threadCount, ready.size());
                std::vector<std::vector<BeamNode>> chunks(workers);
                std::vector<std::thread> threads;
                threads.reserve(workers - 1);
                for (std::size_t worker = 1; worker < workers; ++worker) {
                    const std::size_t begin =
                        ready.size() * worker / workers;
                    const std::size_t end =
                        ready.size() * (worker + 1) / workers;
                    threads.emplace_back(
                        evaluateRange,
                        begin,
                        end,
                        std::ref(chunks[worker]));
                }
                evaluateRange(
                    0, ready.size() / workers, chunks.front());
                for (std::thread& thread : threads) {
                    thread.join();
                }
                for (auto& chunk : chunks) {
                    next.insert(
                        next.end(),
                        std::make_move_iterator(chunk.begin()),
                        std::make_move_iterator(chunk.end()));
                }
            } else {
                evaluateRange(0, ready.size(), next);
            }
            if (evaluationTimedOut.load(std::memory_order_relaxed)) {
                return finish(
                    hasBest ? SearchStopReason::TimeLimit
                            : SearchStopReason::NoValidPlan);
            }
            deduplicateAndLimit(
                next,
                adaptiveLimit(cfg.beamWidth, cfg.minimumBeamWidth));
            beam = std::move(next);
            if (!beam.empty() &&
                (static_cast<std::size_t>(beam.front().state.step) %
                     cfg.incumbentInterval == 0 ||
                 beam.front().state.step ==
                     beam.front().state.dayStepLimit)) {
                consider(beam.front());
            }
            if (expired(true)) {
                return finish(
                    hasBest ? SearchStopReason::TimeLimit
                            : SearchStopReason::NoValidPlan);
            }
        }

        if (beam.empty()) {
            return finish(
                hasBest ? SearchStopReason::Completed
                        : SearchStopReason::NoValidPlan);
        }

        for (BeamNode& node : beam) {
            if (!node.state.finishDay()) {
                node.orderingScore =
                    -std::numeric_limits<double>::infinity();
                continue;
            }
            node.evaluation =
                evaluate(node.state, s, prev, spotDist, cfg.weights);
            node.orderingScore = node.evaluation.total;
        }
        sortAndLimit(beam, 1);
        if (beam.empty() ||
            !std::isfinite(beam.front().orderingScore)) {
            return finish(
                hasBest ? SearchStopReason::Completed
                        : SearchStopReason::NoValidPlan);
        }

        if (validateDailyPlan(s, ms, beam.front().plan, prev).valid) {
            const BeamNode& best = beam.front();
            if (!hasBest || best.evaluation.total > res.evaluation.total ||
                (best.evaluation.total == res.evaluation.total &&
                 best.plan < res.plan)) {
                res.success = true;
                res.plan = compressWaitActions(best.plan);
                res.finalState = best.state;
                res.evaluation = best.evaluation;
            }
        }
        return finish(
            res.success ? SearchStopReason::Completed
                        : SearchStopReason::NoValidPlan);
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
