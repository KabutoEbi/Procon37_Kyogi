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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <queue>
#include <thread>
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

    // 各エージェントで残す合法行動数。0なら全候補を残す。
    std::size_t maxActionsPerAgent = 4;
    bool autoTuneWeights = true;
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
    std::size_t fullEvaluations = 0;
    std::size_t reusedEvaluations = 0;
    SearchStopReason stopReason = SearchStopReason::NoValidPlan;
    std::int64_t elapsedMs = 0;
};

class Brain1 final {
private:
    using DistanceMatrix = std::vector<std::vector<std::int32_t>>;
    using ActionLink = std::uint32_t;
    static constexpr ActionLink kNoAction =
        std::numeric_limits<ActionLink>::max();

    struct SpotDistanceTables {
        DistanceMatrix steps;
        DistanceMatrix fuel;
        std::vector<std::vector<CellPos>> next;
    };

    struct PatrolTarget {
        std::size_t spotIndex = 0;
        std::int32_t priority = 3;
        std::int32_t steps = kImpassableCost;
        bool valid = false;
    };

    struct RendezvousPlan {
        CellPos pos = kInvalidCell;
        AgentIndex supply = kMaxAgents;
        std::int32_t patrolArrival = kImpassableCost;
        std::int32_t supplyArrival = kImpassableCost;
        std::int32_t supplyTravel = kImpassableCost;
        std::int32_t fuelShortage = 0;
        bool required = false;
        bool valid = false;
    };

    struct ActionRecord {
        ActionLink previous = kNoAction;
        std::int32_t action = 0;
    };

    class ActionArena final {
    public:
        explicit ActionArena(std::size_t reserveCount) {
            records_.reserve(reserveCount);
        }

        ActionLink append(ActionLink tail, std::int32_t action) {
            ActionRecord record{tail, action};
            if (isWaitAction(action) && tail != kNoAction &&
                isWaitAction(records_[tail].action)) {
                record.previous = records_[tail].previous;
                record.action += records_[tail].action;
            }
            records_.push_back(record);
            return static_cast<ActionLink>(records_.size() - 1);
        }

        DailyPlan materialize(
            const std::array<ActionLink, kMaxAgents>& tails,
            std::size_t agentCount) const {
            DailyPlan plan(agentCount);
            for (AgentIndex agent = 0; agent < agentCount; ++agent) {
                for (ActionLink link = tails[agent];
                     link != kNoAction;
                     link = records_[link].previous) {
                    plan[agent].push_back(records_[link].action);
                }
                std::reverse(plan[agent].begin(), plan[agent].end());
            }
            return plan;
        }

    private:
        std::vector<ActionRecord> records_;
    };

    class ParallelExecutor final {
    public:
        explicit ParallelExecutor(std::size_t count)
            : count_(std::max<std::size_t>(1, count)) {
            threads_.reserve(count_ - 1);
            for (std::size_t id = 1; id < count_; ++id) {
                threads_.emplace_back([this, id]() { worker(id); });
            }
        }

        ~ParallelExecutor() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
                ++generation_;
            }
            start_.notify_all();
            for (std::thread& thread : threads_) {
                thread.join();
            }
        }

        ParallelExecutor(const ParallelExecutor&) = delete;
        ParallelExecutor& operator=(const ParallelExecutor&) = delete;

        template <class Function>
        void run(std::size_t workers, Function&& function) {
            workers = std::clamp<std::size_t>(workers, 1, count_);
            if (workers == 1) {
                function(0);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                job_ = std::forward<Function>(function);
                active_ = workers;
                completed_ = 0;
                ++generation_;
            }
            start_.notify_all();
            function(0);
            std::unique_lock<std::mutex> lock(mutex_);
            done_.wait(lock, [&]() {
                return completed_ == active_ - 1;
            });
            job_ = {};
        }

    private:
        void worker(std::size_t id) {
            std::size_t observed = 0;
            for (;;) {
                std::function<void(std::size_t)> job;
                std::size_t active = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    start_.wait(lock, [&]() {
                        return stopping_ || generation_ != observed;
                    });
                    if (stopping_) {
                        return;
                    }
                    observed = generation_;
                    job = job_;
                    active = active_;
                }
                if (id < active) {
                    job(id);
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++completed_;
                    if (completed_ == active_ - 1) {
                        done_.notify_one();
                    }
                }
            }
        }

        std::size_t count_ = 1;
        std::vector<std::thread> threads_;
        std::mutex mutex_;
        std::condition_variable start_;
        std::condition_variable done_;
        std::function<void(std::size_t)> job_;
        std::size_t generation_ = 0;
        std::size_t active_ = 1;
        std::size_t completed_ = 0;
        bool stopping_ = false;
    };

    struct BeamNode {
        GameState state{};
        std::array<ActionLink, kMaxAgents> planTails{};
        EvaluationBreakdown evaluation{};
        double orderingScore = 0.0;
        std::size_t stateHash = 0;
        std::uint64_t orderKey = 0;
        std::uint64_t serial = 0;
        bool evaluationDirty = false;

        BeamNode() noexcept {
            planTails.fill(kNoAction);
        }
    };

    struct ActionCandidate {
        GameState state{};
        std::int32_t action = -1;
        double orderingScore = 0.0;
        std::uint64_t orderKey = 0;
    };

    class FlatStateSet final {
    public:
        void reset(std::size_t expected) {
            std::size_t capacity = 8;
            while (capacity < expected * 2) {
                capacity <<= 1U;
            }
            if (slots_.size() < capacity) {
                slots_.resize(capacity);
            }
            activeSize_ = capacity;
            std::fill(
                slots_.begin(),
                slots_.begin() + static_cast<std::ptrdiff_t>(activeSize_),
                kEmpty);
        }

        bool insert(
            const std::vector<BeamNode>& nodes,
            std::size_t index) noexcept {
            const std::size_t mask = activeSize_ - 1;
            std::size_t slot = nodes[index].stateHash & mask;
            for (;;) {
                const std::int32_t stored = slots_[slot];
                if (stored == kEmpty) {
                    slots_[slot] = static_cast<std::int32_t>(index);
                    return true;
                }
                const BeamNode& other =
                    nodes[static_cast<std::size_t>(stored)];
                if (other.stateHash == nodes[index].stateHash &&
                    other.state == nodes[index].state) {
                    return false;
                }
                slot = (slot + 1) & mask;
            }
        }

    private:
        static constexpr std::int32_t kEmpty = -1;
        std::vector<std::int32_t> slots_;
        std::size_t activeSize_ = 0;
    };

    static bool betterNode(const BeamNode& lhs, const BeamNode& rhs) {
        if (lhs.orderingScore != rhs.orderingScore) {
            return lhs.orderingScore > rhs.orderingScore;
        }
        if (lhs.orderKey != rhs.orderKey) {
            return lhs.orderKey < rhs.orderKey;
        }
        return lhs.serial < rhs.serial;
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
        std::size_t limit,
        FlatStateSet& seen,
        std::vector<std::size_t>& indices,
        std::vector<BeamNode>& unique) {
        sortAndLimit(nodes, nodes.size());
        seen.reset(nodes.size());
        indices.clear();
        indices.reserve(std::min(nodes.size(), limit));
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (seen.insert(nodes, i)) {
                indices.push_back(i);
                if (indices.size() == limit) {
                    break;
                }
            }
        }
        unique.clear();
        unique.reserve(std::min(nodes.size(), limit));
        for (const std::size_t index : indices) {
            unique.push_back(std::move(nodes[index]));
        }
        nodes.swap(unique);
        unique.clear();
    }

    // 各スポットへの最短時間と巡回車の最小必要燃料を求める。
    // スポットごとの行は独立しているため、複数スレッドで構築できる。
    static SpotDistanceTables buildSpotDistances(
        const MatchSetting& setting,
        const MatchState& matchState,
        const std::function<bool()>& stop,
        std::size_t threadCount,
        ParallelExecutor& executor,
        bool& stopped) {
        const Field& field = setting.field;
        SpotDistanceTables result;
        result.steps.assign(
            field.spots.size(),
            std::vector<std::int32_t>(
                field.cellCount(), kImpassableCost));
        result.fuel = result.steps;
        result.next.assign(
            field.spots.size(),
            std::vector<CellPos>(field.cellCount(), kInvalidCell));

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
                        if (!fuel) {
                            result.next[spotIndex][
                                static_cast<std::size_t>(from)] = to;
                        }
                        queue.emplace(candidate, from);
                    } else if (!fuel && candidate == current) {
                        CellPos& next = result.next[spotIndex][
                            static_cast<std::size_t>(from)];
                        if (next == kInvalidCell || to < next) {
                            next = to;
                        }
                    }
                }
            }
        };

        auto worker = [&](std::size_t) {
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
        executor.run(workers, worker);
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

    static PatrolTarget selectPatrolTarget(
        const GameState& state,
        AgentIndex agentIndex,
        const MatchSetting& setting,
        const SpotDistanceTables& spotDistances) noexcept {
        PatrolTarget best;
        if (agentIndex >= state.agentCount ||
            !state.agents[agentIndex].isPatrol()) {
            return best;
        }
        const CellPos from = projectedPosition(state, agentIndex);
        if (!setting.field.contains(from)) {
            return best;
        }
        const std::int32_t available = std::max(
            0,
            state.remainingSteps() - projectedDelay(state, agentIndex));
        for (std::size_t i = 0; i < state.spotCount; ++i) {
            if (state.remainingStocks[i] <= 0 ||
                state.hasAcquired(agentIndex, i)) {
                continue;
            }
            const std::int32_t steps =
                spotDistances.steps[i][static_cast<std::size_t>(from)];
            if (steps >= kImpassableCost || steps > available) {
                continue;
            }
            const Spot& spot = setting.field.spots[i];
            const BrandMask bit = brandBit(
                static_cast<std::size_t>(spot.brandIndex));
            const std::int32_t priority =
                (state.score.matchBrands & bit) == 0
                    ? 0
                    : ((state.dayBrands & bit) == 0 ? 1 : 2);
            if (!best.valid || priority < best.priority ||
                (priority == best.priority && steps < best.steps) ||
                (priority == best.priority && steps == best.steps &&
                 i < best.spotIndex)) {
                best = PatrolTarget{i, priority, steps, true};
            }
        }
        return best;
    }

    // スポット上にいて、残り時間内に次の取得対象へ届かない巡回車は
    // 無目的に離れず、その場で日末まで待機する。
    static bool shouldWaitAtSpot(
        const GameState& state,
        AgentIndex agentIndex,
        const MatchSetting& setting,
        const SpotDistanceTables& spotDistances) noexcept {
        if (agentIndex >= state.agentCount ||
            !state.agents[agentIndex].isPatrol()) {
            return false;
        }
        const CellPos pos = projectedPosition(state, agentIndex);
        return setting.field.contains(pos) &&
               setting.field.at(pos).hasSpot() &&
               !selectPatrolTarget(
                    state,
                    agentIndex,
                    setting,
                    spotDistances).valid;
    }

    // スポットまでの最短距離差と六角距離を組み合わせた、任意セル間の
    // 移動時間の軽量な下界。合流候補の比較にだけ使用する。
    static std::int32_t estimatedStepsTo(
        CellPos from,
        CellPos to,
        std::size_t targetSpot,
        const MatchSetting& setting,
        const SpotDistanceTables& spotDistances) noexcept {
        if (from == to) {
            return 0;
        }
        if (!setting.field.contains(from) || !setting.field.contains(to)) {
            return kImpassableCost;
        }
        const std::int32_t hex = hexDistance(setting.field, from, to);
        const auto& toTarget = spotDistances.steps[targetSpot];
        const std::int32_t fromTarget =
            toTarget[static_cast<std::size_t>(from)];
        const std::int32_t cellTarget =
            toTarget[static_cast<std::size_t>(to)];
        if (hex >= kImpassableCost || fromTarget >= kImpassableCost ||
            cellTarget >= kImpassableCost) {
            return hex;
        }
        return std::max(hex, std::max(0, fromTarget - cellTarget));
    }

    // 巡回車の最短時間経路を燃料が許す位置までたどり、巡回車を
    // 待たせない（または待ち時間が最小の）経路上合流地点を選ぶ。
    static RendezvousPlan findRendezvous(
        const GameState& state,
        AgentIndex patrolIndex,
        const PatrolTarget& target,
        const MatchSetting& setting,
        const MatchState& matchState,
        const SpotDistanceTables& spotDistances,
        AgentIndex onlySupply = kMaxAgents) noexcept {
        RendezvousPlan best;
        if (!target.valid || patrolIndex >= state.agentCount) {
            return best;
        }
        const Agent& patrol = state.agents[patrolIndex];
        const CellPos start = projectedPosition(state, patrolIndex);
        if (!patrol.isPatrol() || !setting.field.contains(start)) {
            return best;
        }

        const PendingAction& patrolPending =
            state.pendingActions[patrolIndex];
        std::int32_t fuel = patrol.fuel;
        if (patrolPending.isMove()) {
            fuel -= patrolPending.fuelCost;
        }
        fuel = std::max(0, fuel);
        const std::int32_t requiredFuel = spotDistances.fuel[
            target.spotIndex][static_cast<std::size_t>(start)];
        const std::int32_t threshold = std::max(
            1, (setting.fuelLimit + 1) / 2);
        best.required = requiredFuel > fuel;
        best.fuelShortage = best.required && requiredFuel < kImpassableCost
                                ? requiredFuel - fuel
                                : std::max(0, threshold - fuel);
        if (!best.required && fuel >= threshold) {
            return best;
        }

        CellPos cell = start;
        std::int32_t patrolArrival = projectedDelay(state, patrolIndex);
        std::int32_t fuelLeft = fuel;
        const CellPos goal = setting.field.spots[target.spotIndex].pos;
        const auto& next = spotDistances.next[target.spotIndex];

        for (std::size_t guard = 0;
             guard <= setting.field.cellCount();
             ++guard) {
            const std::int32_t remainingRoute = spotDistances.steps[
                target.spotIndex][static_cast<std::size_t>(cell)];
            for (AgentIndex supplyIndex = 0;
                 supplyIndex < state.agentCount;
                 ++supplyIndex) {
                if (!state.agents[supplyIndex].isSupply() ||
                    (onlySupply < kMaxAgents && supplyIndex != onlySupply)) {
                    continue;
                }
                const CellPos supplyPos =
                    projectedPosition(state, supplyIndex);
                const std::int32_t travel = estimatedStepsTo(
                    supplyPos,
                    cell,
                    target.spotIndex,
                    setting,
                    spotDistances);
                if (travel >= kImpassableCost) {
                    continue;
                }
                const std::int32_t supplyArrival =
                    projectedDelay(state, supplyIndex) + travel;
                const std::int32_t meetAt =
                    std::max(patrolArrival, supplyArrival);
                if (remainingRoute >= kImpassableCost ||
                    meetAt > state.remainingSteps() - remainingRoute) {
                    continue;
                }
                const std::int32_t wait =
                    std::max(0, supplyArrival - patrolArrival);
                const std::int32_t bestWait = best.valid
                    ? std::max(0, best.supplyArrival - best.patrolArrival)
                    : kImpassableCost;
                if (!best.valid || wait < bestWait ||
                    (wait == bestWait &&
                     patrolArrival > best.patrolArrival) ||
                    (wait == bestWait &&
                     patrolArrival == best.patrolArrival &&
                     travel < best.supplyTravel) ||
                    (wait == bestWait &&
                     patrolArrival == best.patrolArrival &&
                     travel == best.supplyTravel &&
                     supplyIndex < best.supply)) {
                    best.pos = cell;
                    best.supply = supplyIndex;
                    best.patrolArrival = patrolArrival;
                    best.supplyArrival = supplyArrival;
                    best.supplyTravel = travel;
                    best.valid = true;
                }
            }

            if (cell == goal) {
                break;
            }
            const CellPos following = next[static_cast<std::size_t>(cell)];
            if (!setting.field.contains(following)) {
                break;
            }
            const std::int32_t edgeFuel = moveFuelCost(
                setting, AgentKind::Patrol, cell);
            const std::int32_t edgeSteps = moveStepCost(
                setting, matchState, cell);
            if (edgeFuel > fuelLeft || edgeSteps >= kImpassableCost) {
                break;
            }
            fuelLeft -= edgeFuel;
            patrolArrival += edgeSteps;
            cell = following;
        }
        return best;
    }

    static EvaluationContext makeEvaluationContext(
        const GameState& state,
        const MatchSetting& setting,
        const MatchState& matchState,
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
            bool hasCandidate = false;
            bool hasTimeReachableCandidate = false;
            std::int32_t projectedFuel = agent.fuel;
            if (state.pendingActions[agentIndex].isMove()) {
                projectedFuel -=
                    state.pendingActions[agentIndex].fuelCost;
            }
            projectedFuel = std::max(0, projectedFuel);
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
                if (fuel > projectedFuel) {
                    continue;
                }
                const Spot& spot = setting.field.spots[spotIndex];
                const BrandMask bit = brandBit(
                    static_cast<std::size_t>(spot.brandIndex));
                const bool newMatch = (state.score.matchBrands & bit) == 0;
                const bool newDay = (state.dayBrands & bit) == 0;
                const std::int32_t total = reservedSteps + distance;
                nearestSpot = std::min(nearestSpot, total);
                if (newMatch) {
                    nearestNew = std::min(nearestNew, total);
                }
                if (newDay) {
                    nearestDaily = std::min(nearestDaily, total);
                }
            }

            const PatrolTarget target = selectPatrolTarget(
                state, agentIndex, setting, spotDistances);
            const bool waitsAtSpot = shouldWaitAtSpot(
                state, agentIndex, setting, spotDistances);
            if (target.valid) {
                targets[agentIndex] =
                    static_cast<std::int32_t>(target.spotIndex);
            }
            const RendezvousPlan rendezvous = findRendezvous(
                state,
                agentIndex,
                target,
                setting,
                matchState,
                spotDistances);
            const bool directTarget = target.valid &&
                spotDistances.fuel[target.spotIndex][
                    static_cast<std::size_t>(projected)] <= projectedFuel;

            // 補給込みで到達できる目標も距離評価へ戻し、巡回車が
            // 合流地点とスポットへ進む枝を残す。
            if (target.valid && !directTarget && rendezvous.valid) {
                const std::int32_t total = reservedSteps + target.steps;
                nearestSpot = std::min(nearestSpot, total);
                if (target.priority == 0) {
                    nearestNew = std::min(nearestNew, total);
                }
                if (target.priority <= 1) {
                    nearestDaily = std::min(nearestDaily, total);
                }
            }
            context.nearestValuableSpotSteps[agentIndex] = nearestSpot;
            context.nearestNewBrandSpotSteps[agentIndex] = nearestNew;
            context.nearestDailyBrandSpotSteps[agentIndex] = nearestDaily;
            if (target.valid && (directTarget || rendezvous.valid)) {
                const std::int32_t wait = rendezvous.valid
                    ? std::max(
                          0,
                          rendezvous.supplyArrival -
                              rendezvous.patrolArrival)
                    : 0;
                context.targetSlack += std::max(
                    0, availableSteps - target.steps - wait);
            } else if (hasCandidate && !waitsAtSpot) {
                ++context.unreachableTargetCount;
                if (hasTimeReachableCandidate) {
                    context.fuelRisk += std::max(
                        1,
                        (setting.fuelLimit + 1) / 2 - projectedFuel);
                }
            }

            const std::int32_t fuelThreshold = std::max(
                1, (setting.fuelLimit + 1) / 2);
            const std::int32_t fuelNeed =
                std::max(0, fuelThreshold - projectedFuel);
            if (rendezvous.valid &&
                (rendezvous.required || fuelNeed > 0)) {
                context.nearestSupplySteps[agentIndex] =
                    rendezvous.supplyTravel;
                context.rendezvousMismatch += std::max(
                    0,
                    rendezvous.supplyArrival - rendezvous.patrolArrival);
                context.fuelRisk += std::max(
                    fuelNeed, rendezvous.fuelShortage);
            } else if (waitsAtSpot &&
                       projectedFuel < setting.fuelLimit) {
                const std::size_t currentSpot = static_cast<std::size_t>(
                    setting.field.at(projected).spotIndex);
                std::int32_t supplyArrival = kImpassableCost;
                for (AgentIndex other = 0;
                     other < state.agentCount;
                     ++other) {
                    if (!state.agents[other].isSupply()) {
                        continue;
                    }
                    const CellPos supplyPos =
                        projectedPosition(state, other);
                    const std::int32_t travel = spotDistances.steps[
                        currentSpot][static_cast<std::size_t>(supplyPos)];
                    if (travel >= kImpassableCost) {
                        continue;
                    }
                    supplyArrival = std::min(
                        supplyArrival,
                        projectedDelay(state, other) + travel);
                }
                if (supplyArrival <= state.remainingSteps()) {
                    context.nearestSupplySteps[agentIndex] = supplyArrival;
                    context.fuelRisk +=
                        setting.fuelLimit - projectedFuel;
                }
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
        const MatchState& matchState,
        ScoreState scoreAtDayStart,
        const SpotDistanceTables& spotDistances,
        const EvaluationWeights& weights) {
        const EvaluationContext context = makeEvaluationContext(
            state,
            setting,
            matchState,
            scoreAtDayStart,
            spotDistances);
        return evaluateGameState(state, context, weights);
    }

    static EvaluationWeights tunedWeights(
        const MatchSetting& setting,
        const MatchState& state,
        const BeamSearchConfig& config) noexcept {
        EvaluationWeights weights = config.weights;
        if (!config.autoTuneWeights) {
            return weights;
        }

        const std::int32_t steps = setting.daySteps[
            static_cast<std::size_t>(state.day)];
        const double fuelRatio = static_cast<double>(setting.fuelLimit) /
                                 std::max(1, steps);
        const double fuelPressure = std::clamp(
            (1.5 - fuelRatio) / 1.5, 0.0, 1.0);
        const double mapScale = std::sqrt(
            static_cast<double>(setting.field.cellCount()) / 256.0);
        const double patrolScale = std::max(
            1.0,
            static_cast<double>(std::count_if(
                state.agents.begin(),
                state.agents.end(),
                [](const Agent& agent) { return agent.isPatrol(); })) /
                4.0);

        weights.valuableSpotDistance /= std::max(1.0, mapScale);
        weights.newBrandSpotDistance /= std::max(1.0, mapScale);
        weights.dailyBrandSpotDistance /= std::max(1.0, mapScale);
        weights.supplyDistance *= 1.0 + 1.5 * fuelPressure;
        weights.rendezvousMismatch *= 1.0 + fuelPressure;
        weights.fuelRisk *= 1.0 + 2.0 * fuelPressure;
        weights.concentration *= patrolScale;
        return weights;
    }

    static bool canReuseEvaluation(const BeamNode& node) noexcept {
        if (node.evaluationDirty) {
            return false;
        }
        for (AgentIndex i = 0; i < node.state.agentCount; ++i) {
            if (!node.state.pendingActions[i].active ||
                node.state.pendingActions[i].remainingSteps <= 1) {
                return false;
            }
        }
        return true;
    }

    static void advanceCachedEvaluation(
        EvaluationBreakdown& evaluation,
        const EvaluationWeights& weights) noexcept {
        --evaluation.unusedSteps;
        evaluation.valuableSpotDistance -=
            evaluation.valuableSpotAgentCount;
        evaluation.newBrandSpotDistance -=
            evaluation.newBrandSpotAgentCount;
        evaluation.dailyBrandSpotDistance -=
            evaluation.dailyBrandSpotAgentCount;
        evaluation.total = evaluationTotal(evaluation, weights);
    }

    static std::uint64_t appendOrderKey(
        std::uint64_t key,
        AgentIndex agent,
        std::int32_t action) noexcept {
        key ^= static_cast<std::uint64_t>(agent + 1) *
               UINT64_C(0x9e3779b97f4a7c15);
        key ^= static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(action + 17));
        key *= UINT64_C(1099511628211);
        return key;
    }

    static double scheduledActionBonus(
        const GameState& state,
        AgentIndex agentIndex,
        const MatchSetting& setting,
        const MatchState& matchState,
        const SpotDistanceTables& spotDistances,
        const EvaluationWeights& weights) noexcept {
        const Agent& agent = state.agents[agentIndex];
        const PendingAction& pending =
            state.pendingActions[agentIndex];
        if (!pending.active) {
            return 0.0;
        }

        if (!agent.isPatrol()) {
            double bonus = pending.isWait()
                               ? -1.0
                               : -0.01 * pending.remainingSteps;
            double bestGuidance =
                -std::numeric_limits<double>::infinity();
            for (AgentIndex patrolIndex = 0;
                 patrolIndex < state.agentCount;
                 ++patrolIndex) {
                if (!state.agents[patrolIndex].isPatrol()) {
                    continue;
                }
                if (shouldWaitAtSpot(
                        state,
                        patrolIndex,
                        setting,
                        spotDistances) &&
                    state.agents[patrolIndex].fuel < setting.fuelLimit) {
                    const CellPos patrolPos =
                        projectedPosition(state, patrolIndex);
                    const std::size_t spotIndex =
                        static_cast<std::size_t>(
                            setting.field.at(patrolPos).spotIndex);
                    const CellPos supplyPos =
                        projectedPosition(state, agentIndex);
                    const std::int32_t travel = spotDistances.steps[
                        spotIndex][static_cast<std::size_t>(supplyPos)];
                    const std::int32_t arrival =
                        projectedDelay(state, agentIndex) + travel;
                    if (travel < kImpassableCost &&
                        arrival <= state.remainingSteps()) {
                        bestGuidance = std::max(
                            bestGuidance,
                            -weights.fuelRisk * 0.5 +
                                weights.supplyDistance * arrival);
                    }
                }
                const PatrolTarget target = selectPatrolTarget(
                    state, patrolIndex, setting, spotDistances);
                const RendezvousPlan rendezvous = findRendezvous(
                    state,
                    patrolIndex,
                    target,
                    setting,
                    matchState,
                    spotDistances,
                    agentIndex);
                if (!rendezvous.valid) {
                    continue;
                }
                const std::int32_t wait = std::max(
                    0,
                    rendezvous.supplyArrival -
                        rendezvous.patrolArrival);
                const double urgency = rendezvous.required
                    ? -weights.fuelRisk * 2.0
                    : -weights.fuelRisk * 0.25;
                bestGuidance = std::max(
                    bestGuidance,
                    urgency +
                        weights.supplyDistance *
                            rendezvous.supplyTravel +
                        weights.rendezvousMismatch * wait);
            }
            if (std::isfinite(bestGuidance)) {
                bonus += bestGuidance;
            }
            return bonus;
        }

        const CellPos projected = pending.isMove()
                                      ? pending.destination
                                      : agent.pos;
        std::int32_t nearest = kImpassableCost;
        std::int32_t priority = 3;
        for (std::size_t spotIndex = 0;
             spotIndex < state.spotCount;
             ++spotIndex) {
            if (state.remainingStocks[spotIndex] <= 0 ||
                state.hasAcquired(agentIndex, spotIndex)) {
                continue;
            }
            const std::size_t pos = static_cast<std::size_t>(projected);
            const std::int32_t distance =
                spotDistances.steps[spotIndex][pos];
            const Spot& spot = setting.field.spots[spotIndex];
            const BrandMask bit = brandBit(
                static_cast<std::size_t>(spot.brandIndex));
            const std::int32_t kind =
                (state.score.matchBrands & bit) == 0
                    ? 0
                    : ((state.dayBrands & bit) == 0 ? 1 : 2);
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
            if (state.remainingStocks[spotIndex] > 0 &&
                !state.hasAcquired(agentIndex, spotIndex)) {
                const Spot& spot = setting.field.spots[spotIndex];
                const BrandMask bit = brandBit(
                    static_cast<std::size_t>(spot.brandIndex));
                bonus += (state.score.matchBrands & bit) == 0
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
        std::atomic<std::size_t> fullEvaluationCount{0};
        std::atomic<std::size_t> reusedEvaluationCount{0};
        auto finish = [&](SearchStopReason reason) {
            if (res.success) {
                res.plan = compressWaitActions(res.plan);
            }
            res.stopReason = reason;
            res.elapsedMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(Clock::now() - started).count();
            res.fullEvaluations =
                fullEvaluationCount.load(std::memory_order_relaxed);
            res.reusedEvaluations =
                reusedEvaluationCount.load(std::memory_order_relaxed);
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

        const EvaluationWeights weights = tunedWeights(s, ms, cfg);
        ParallelExecutor executor(threadCount);
        constexpr std::size_t kArenaReserveLimit = 4'000'000;
        auto cappedProduct = [=](std::size_t lhs, std::size_t rhs) {
            return lhs == 0 || rhs == 0
                       ? std::size_t{0}
                       : (lhs > kArenaReserveLimit / rhs
                              ? kArenaReserveLimit
                              : lhs * rhs);
        };
        std::size_t arenaReserve = std::min(
            cfg.jointBeamWidth, kArenaReserveLimit);
        arenaReserve = cappedProduct(
            arenaReserve,
            static_cast<std::size_t>(std::max(1, init.dayStepLimit)));
        arenaReserve = cappedProduct(
            arenaReserve, std::max<std::size_t>(1, init.agentCount));
        ActionArena actionArena(arenaReserve);
        FlatStateSet stateSet;
        std::vector<std::size_t> uniqueIndices;
        std::vector<BeamNode> dedupScratch;
        std::vector<std::vector<BeamNode>> workerChunks(threadCount);
        std::uint64_t nextSerial = 1;

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
                fallback.state, ctx, weights);
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
            executor,
            prepStopped);
        if (prepStopped) {
            return finish(
                hasBest ? SearchStopReason::TimeLimit
                        : SearchStopReason::NoValidPlan);
        }
        BeamNode root;
        root.state = init;
        root.evaluation =
            evaluate(root.state, s, ms, prev, spotDist, weights);
        fullEvaluationCount.fetch_add(1, std::memory_order_relaxed);
        root.orderingScore = root.evaluation.total;
        root.stateHash = GameStateHash{}(root.state);
        root.orderKey = UINT64_C(1469598103934665603);

        auto consider = [&](const BeamNode& node) {
            DailyPlan plan = actionArena.materialize(
                node.planTails, node.state.agentCount);
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
                sim.state, s, ms, prev, spotDist, weights);
            fullEvaluationCount.fetch_add(1, std::memory_order_relaxed);
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
                res.finalState, s, ms, prev, spotDist, weights);
            fullEvaluationCount.fetch_add(1, std::memory_order_relaxed);
        }
        consider(root);
        if (expired(true)) {
            return finish(
                hasBest ? SearchStopReason::TimeLimit
                        : SearchStopReason::NoValidPlan);
        }

        std::vector<BeamNode> beam{std::move(root)};
        std::vector<BeamNode> ready;
        std::vector<BeamNode> staged;
        std::vector<BeamNode> advanced;
        ready.reserve(cfg.jointBeamWidth);
        staged.reserve(cfg.jointBeamWidth * 2);
        advanced.reserve(cfg.jointBeamWidth);

        while (!beam.empty() &&
               beam.front().state.step <
                   beam.front().state.dayStepLimit) {
            ready.clear();
            ready.swap(beam);

            // 待機中でない各エージェントへ、合法な6方向移動または
            // 1ステップ待機を割り当てる。
            for (AgentIndex a = 0; a < init.agentCount; ++a) {
                staged.clear();
                staged.reserve(std::min(
                    ready.size() * 7,
                    cfg.jointBeamWidth * 2));
                for (const BeamNode& node : ready) {
                    if (node.state.pendingActions[a].active) {
                        staged.push_back(node);
                        continue;
                    }

                    constexpr std::array<std::int32_t, 7> actions = {
                        0, 1, 2, 3, 4, 5, -1,
                    };
                    const bool forceWait = shouldWaitAtSpot(
                        node.state, a, s, spotDist);
                    std::array<ActionCandidate, actions.size()> candidates{};
                    std::size_t candidateCount = 0;
                    for (const std::int32_t action : actions) {
                        if (expired()) {
                            return finish(
                                hasBest ? SearchStopReason::TimeLimit
                                        : SearchStopReason::NoValidPlan);
                        }
                        if ((forceWait && !isWaitAction(action)) ||
                            !canScheduleAction(
                                node.state, s, ms, a, action)) {
                            continue;
                        }
                        ActionCandidate& candidate =
                            candidates[candidateCount];
                        candidate.state = node.state;
                        const auto err = tryScheduleAction(
                            candidate.state,
                            s,
                            ms,
                            a,
                            action,
                            0);
                        if (err.error != SimulationError::None) {
                            continue;
                        }
                        candidate.action = action;
                        candidate.orderKey = appendOrderKey(
                            node.orderKey, a, action);
                        candidate.orderingScore =
                            node.orderingScore +
                            scheduledActionBonus(
                                candidate.state,
                                a,
                                s,
                                ms,
                                spotDist,
                                weights);
                        ++candidateCount;
                        ++res.expandedNodes;
                    }

                    const std::size_t keep =
                        cfg.maxActionsPerAgent == 0
                            ? candidateCount
                            : std::min(
                                  candidateCount,
                                  cfg.maxActionsPerAgent);
                    std::partial_sort(
                        candidates.begin(),
                        candidates.begin() +
                            static_cast<std::ptrdiff_t>(keep),
                        candidates.begin() +
                            static_cast<std::ptrdiff_t>(candidateCount),
                        [](const ActionCandidate& lhs,
                           const ActionCandidate& rhs) {
                            if (lhs.orderingScore != rhs.orderingScore) {
                                return lhs.orderingScore > rhs.orderingScore;
                            }
                            if (lhs.orderKey != rhs.orderKey) {
                                return lhs.orderKey < rhs.orderKey;
                            }
                            return lhs.action < rhs.action;
                        });
                    for (std::size_t i = 0; i < keep; ++i) {
                        const ActionCandidate& candidate = candidates[i];
                        BeamNode child = node;
                        child.state = candidate.state;
                        child.planTails[a] = actionArena.append(
                            node.planTails[a], candidate.action);
                        child.orderingScore = candidate.orderingScore;
                        child.orderKey = candidate.orderKey;
                        child.serial = nextSerial++;
                        child.evaluationDirty = true;
                        staged.push_back(std::move(child));
                    }
                }
                sortAndLimit(
                    staged,
                    adaptiveLimit(
                        cfg.jointBeamWidth,
                        cfg.minimumJointBeamWidth));
                ready.clear();
                ready.swap(staged);
                if (ready.empty()) {
                    return finish(
                        hasBest ? SearchStopReason::Completed
                                : SearchStopReason::NoValidPlan);
                }
            }

            advanced.clear();
            advanced.reserve(ready.size());
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
                    const bool reuse = canReuseEvaluation(node);
                    const auto err = advanceSimulationStep(node.state, s);
                    if (err.error != SimulationError::None) {
                        continue;
                    }
                    if (reuse) {
                        advanceCachedEvaluation(node.evaluation, weights);
                        reusedEvaluationCount.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        node.evaluation = evaluate(
                            node.state, s, ms, prev, spotDist, weights);
                        fullEvaluationCount.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    node.evaluationDirty = false;
                    node.orderingScore = node.evaluation.total;
                    node.stateHash = GameStateHash{}(node.state);
                    output.push_back(std::move(node));
                }
            };

            if (parallel) {
                const std::size_t workers = std::min(
                    threadCount, ready.size());
                for (std::size_t worker = 0;
                     worker < workers;
                     ++worker) {
                    workerChunks[worker].clear();
                }
                executor.run(workers, [&](std::size_t worker) {
                    const std::size_t begin =
                        ready.size() * worker / workers;
                    const std::size_t end =
                        ready.size() * (worker + 1) / workers;
                    evaluateRange(begin, end, workerChunks[worker]);
                });
                for (std::size_t worker = 0;
                     worker < workers;
                     ++worker) {
                    auto& chunk = workerChunks[worker];
                    advanced.insert(
                        advanced.end(),
                        std::make_move_iterator(chunk.begin()),
                        std::make_move_iterator(chunk.end()));
                }
            } else {
                evaluateRange(0, ready.size(), advanced);
            }
            if (evaluationTimedOut.load(std::memory_order_relaxed)) {
                return finish(
                    hasBest ? SearchStopReason::TimeLimit
                            : SearchStopReason::NoValidPlan);
            }
            deduplicateAndLimit(
                advanced,
                adaptiveLimit(cfg.beamWidth, cfg.minimumBeamWidth),
                stateSet,
                uniqueIndices,
                dedupScratch);
            beam.swap(advanced);
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
                evaluate(node.state, s, ms, prev, spotDist, weights);
            fullEvaluationCount.fetch_add(1, std::memory_order_relaxed);
            node.orderingScore = node.evaluation.total;
        }
        sortAndLimit(beam, 1);
        if (beam.empty() ||
            !std::isfinite(beam.front().orderingScore)) {
            return finish(
                hasBest ? SearchStopReason::Completed
                        : SearchStopReason::NoValidPlan);
        }

        const DailyPlan finalPlan = actionArena.materialize(
            beam.front().planTails, beam.front().state.agentCount);
        if (validateDailyPlan(s, ms, finalPlan, prev).valid) {
            const BeamNode& best = beam.front();
            if (!hasBest || best.evaluation.total > res.evaluation.total ||
                (best.evaluation.total == res.evaluation.total &&
                 finalPlan < res.plan)) {
                res.success = true;
                res.plan = finalPlan;
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
