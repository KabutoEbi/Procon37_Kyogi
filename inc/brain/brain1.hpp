#pragma once

#include "brain0.hpp"
#include "../action_routes.hpp"
#include "../evaluation.hpp"
#include "../game_state_hash.hpp"
#include "../min_cost_flow.hpp"
#include "../plan_validator.hpp"
#include "../search_state.hpp"
#include "../simulator.hpp"

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
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace benzen {

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
    // 燃料制約の厳しい問題では未実行マクロも完了させ、待機案と比較する。
    bool completeIncumbentRoutes = true;

    // 各エージェントで残す合法行動数。0なら全候補を残す。
    std::size_t maxActionsPerAgent = 4;
    bool adaptiveActionLimit = true;
    bool autoTuneWeights = true;
    // 燃料制約が厳しく、有効予算300ms以下の場合だけ使う重み補正。
    bool fuelAwareWeights = true;
    bool useMacroActions = true;
    std::size_t macroTargetLimit = 3;
    // マクロ継続を残しつつ、定期的に別経路も探索する。0なら再検討しない。
    std::size_t macroReplanInterval = 0;
    bool coordinatedAssignments = true;
    bool detourRendezvous = true;
    bool twoSpotLookahead = true;
    std::size_t assignmentCacheLimit = 4096;
    // 同一地形・交通条件の距離前計算を呼び出しスレッド内で再利用する。
    // 0なら無効。状態や締切・割当キャッシュは保存しない。
    std::size_t distanceCacheEntries = 8;
    std::size_t detourCandidateLimit = 6;
    std::size_t lookaheadCandidateLimit = 6;
    bool profilePlanning = false;
    bool dominancePruning = true;
    // 上位解を残しつつ、この割合を異なる系列・位置の候補へ確保する。
    double diversityFraction = 0.25;
};

enum class SearchStopReason : std::uint8_t {
    Completed,
    TimeLimit,
    NoValidPlan,
};

struct BeamSearchResult {
    bool reusedDistanceTables = false;
    bool success = false;
    DailyPlan plan;
    GameState finalState{};
    EvaluationBreakdown evaluation{};
    std::size_t expandedNodes = 0;
    std::size_t fullEvaluations = 0;
    std::size_t reusedEvaluations = 0;
    std::size_t macroCandidates = 0;
    std::size_t macroContinuations = 0;
    std::size_t assignmentCacheHits = 0;
    std::size_t assignmentComputations = 0;
    std::size_t repairedAssignments = 0;
    std::size_t detourPlans = 0;
    std::int64_t assignmentTimeNs = 0;
    std::int64_t flowTimeNs = 0;
    std::int64_t rendezvousTimeNs = 0;
    SearchStopReason stopReason = SearchStopReason::NoValidPlan;
    std::int64_t elapsedMs = 0;
};

class Brain1 final {
private:
    using DistanceMatrix = std::vector<std::vector<std::int32_t>>;
    using ActionLink = std::uint32_t;
    static constexpr ActionLink kNoAction =
        std::numeric_limits<ActionLink>::max();

    struct PathLabel {
        std::int32_t steps = kImpassableCost;
        std::int32_t fuel = kImpassableCost;
        CellPos next = kInvalidCell;
    };

    struct AssignmentCache;
    struct PlanningTimings {
        std::atomic<std::int64_t> assignment{0}, flow{0}, rendezvous{0};
    };
    class PlanningTimer {
        std::atomic<std::int64_t>* counter_;
        std::chrono::steady_clock::time_point started_;
    public:
        explicit PlanningTimer(std::atomic<std::int64_t>* counter)
            : counter_(counter) {
            if (counter_) started_ = std::chrono::steady_clock::now();
        }
        ~PlanningTimer() {
            if (!counter_) return;
            const auto elapsed = std::chrono::steady_clock::now() - started_;
            const auto ns = std::chrono::duration_cast<
                std::chrono::nanoseconds>(elapsed).count();
            counter_->fetch_add(ns, std::memory_order_relaxed);
        }
    };
    struct SpotDistanceTables {
        DistanceMatrix travel;
        std::vector<std::vector<CellPos>> firstHop;
        DistanceMatrix steps;
        DistanceMatrix fuel;
        std::vector<std::vector<std::vector<PathLabel>>> pareto;
        BeamSearchConfig planning{};
        std::shared_ptr<AssignmentCache> assignments;
        std::shared_ptr<PlanningTimings> timings;
        std::function<bool()> stop;
        std::vector<std::vector<std::size_t>> nextSpots;
    };

    struct DistanceCacheKey {
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::vector<CellType> terrain;
        std::vector<std::int32_t> stepCosts;
        std::vector<CellPos> spotPositions;

        bool operator==(const DistanceCacheKey& other) const noexcept {
            return width == other.width && height == other.height &&
                   terrain == other.terrain && stepCosts == other.stepCosts &&
                   spotPositions == other.spotPositions;
        }
    };

    struct CachedDistances {
        DistanceCacheKey key;
        SpotDistanceTables tables;
    };

    static std::vector<CachedDistances>& distanceCache() {
        static thread_local std::vector<CachedDistances> cache;
        return cache;
    }

    static DistanceCacheKey makeDistanceCacheKey(
        const MatchSetting& setting, const MatchState& matchState) {
        const Field& field = setting.field;
        DistanceCacheKey key;
        key.width = field.width;
        key.height = field.height;
        key.terrain.reserve(field.cellCount());
        key.stepCosts.reserve(field.cellCount());
        for (CellPos pos = 0;
             pos < static_cast<CellPos>(field.cellCount()); ++pos) {
            key.terrain.push_back(field.at(pos).type);
            key.stepCosts.push_back(moveStepCost(setting, matchState, pos));
        }
        key.spotPositions.reserve(field.spots.size());
        for (const auto& spot : field.spots)
            key.spotPositions.push_back(spot.pos);
        return key;
    }

    static bool restoreCachedDistances(
        const DistanceCacheKey& key, std::size_t capacity,
        SpotDistanceTables& result) {
        auto& cache = distanceCache();
        while (cache.size() > capacity) cache.erase(cache.begin());
        for (std::size_t i = 0; i < cache.size(); ++i) {
            if (!(cache[i].key == key)) continue;
            std::rotate(cache.begin() + static_cast<std::ptrdiff_t>(i),
                        cache.begin() + static_cast<std::ptrdiff_t>(i + 1),
                        cache.end());
            result = cache.back().tables;
            return true;
        }
        return false;
    }

    static void storeCachedDistances(
        DistanceCacheKey key, const SpotDistanceTables& tables,
        std::size_t capacity) {
        if (capacity == 0) return;
        auto& cache = distanceCache();
        if (cache.size() == capacity) cache.erase(cache.begin());
        cache.push_back({std::move(key), tables});
    }

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
        std::int32_t goalArrival = kImpassableCost;
        PathLabel path{};
        // 目的地への非劣経路から外れて合流する場合の巡回車経路。
        std::shared_ptr<const AgentPlan> detourPrefix;
        bool required = false;
        bool needsRefuelAgain = false;
        bool valid = false;
    };

    // 1回の共同割当内では位置・燃料・予約・残時間は変わらない。
    // 仮取得で変わる在庫・得点は合流経路計算には使われない。
    struct RendezvousMemo {
        static constexpr std::uint16_t missing = std::numeric_limits<std::uint16_t>::max();
        std::array<std::uint16_t, kMaxAgents * kMaxSpots * (kMaxAgents + 1)> indices{};
        std::vector<RendezvousPlan> plans;
        AgentIndex soleSupply = kMaxAgents;

        void reset(const GameState& state) {
            indices.fill(missing);
            plans.clear();
            soleSupply = kMaxAgents;
            std::size_t count = 0;
            for (AgentIndex a = 0; a < state.agentCount; ++a)
                if (state.agents[a].isSupply()) { soleSupply = a; ++count; }
            if (count != 1) soleSupply = kMaxAgents;
        }
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
            std::function<void(std::size_t)> task(
                std::forward<Function>(function));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                job_ = task;
                active_ = workers;
                completed_ = 0;
                ++generation_;
            }
            start_.notify_all();
            task(0);
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
        std::array<ActionRoutes::Id, kMaxAgents> routes{};
        std::array<CellPos, kMaxAgents> aims{};
        double orderingScore = 0.0;
        std::uint64_t orderKey = 0;
        std::uint64_t serial = 0;

        BeamNode() noexcept {
            planTails.fill(kNoAction);
            routes.fill(ActionRoutes::none);
            aims.fill(kInvalidCell);
        }
    };

    struct ActionCandidate {
        PendingAction pending{};
        std::int32_t action = -1;
        double orderingScore = 0.0;
        std::uint64_t orderKey = 0;
        ActionRoutes::Id route = ActionRoutes::none;
        CellPos aim = kInvalidCell;
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

    static bool preferPlan(const DailyPlan& lhs, const DailyPlan& rhs) {
        auto initialWait = [](const DailyPlan& plan) {
            std::int64_t total = 0;
            for (const auto& actions : plan) {
                for (const int action : actions) {
                    if (!isWaitAction(action)) break;
                    total -= action;
                }
            }
            return total;
        };
        const auto left = initialWait(lhs), right = initialWait(rhs);
        return left != right ? left < right : lhs < rhs;
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
        std::vector<BeamNode>& nodes, std::size_t limit,
        const Field& field, const BeamSearchConfig& config,
        std::vector<std::size_t>& indices, std::vector<BeamNode>& unique) {
        sortAndLimit(nodes, nodes.size());
        indices.clear();
        if (limit == 0) { nodes.clear(); return; }
        static thread_local std::vector<GameState> shapes;
        shapes.clear();
        shapes.reserve(nodes.size());
        static thread_local std::vector<std::uint8_t> removed;
        removed.assign(nodes.size(), 0);
        const std::size_t none = nodes.size();
        struct Group {
            std::size_t hash;
            std::size_t first;
            std::size_t last;
            bool occupied;
        };
        std::size_t groupSlots = 1;
        while (groupSlots < nodes.size() * 2) groupSlots *= 2;
        static thread_local std::vector<Group> groups;
        groups.assign(groupSlots, Group{0, none, none, false});
        static thread_local std::vector<std::size_t> nextInGroup;
        nextInGroup.assign(nodes.size(), none);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            shapes.push_back(canonicalSearchState(nodes[i].state, config.dominancePruning));
            std::size_t hash = GameStateHash{}(shapes.back());
            for (const auto route : nodes[i].routes) detail::hashValue(hash, route);
            std::size_t slot = hash & (groupSlots - 1);
            while (groups[slot].occupied && groups[slot].hash != hash)
                slot = (slot + 1) & (groupSlots - 1);
            auto& group = groups[slot];
            if (!group.occupied) {
                group.hash = hash;
                group.occupied = true;
            }
            for (std::size_t j = group.first; j != none; j = nextInGroup[j]) {
                if (removed[j] || nodes[i].routes != nodes[j].routes ||
                    shapes[i] != shapes[j]) continue;
                if (!config.dominancePruning ||
                    fuelDominates(nodes[j].state, nodes[i].state)) {
                    removed[i] = true;
                    break;
                }
                if (fuelDominates(nodes[i].state, nodes[j].state)) removed[j] = true;
            }
            if (!removed[i]) {
                if (group.last == none) group.first = i;
                else nextInGroup[group.last] = i;
                group.last = i;
            }
        }
        static thread_local std::vector<std::uint8_t> chosen;
        chosen.assign(nodes.size(), 0);
        const std::size_t elite = std::max<std::size_t>(
            1, limit - static_cast<std::size_t>(limit * config.diversityFraction));
        auto signature = [&](const BeamNode& node) {
            std::size_t hash = 0;
            detail::hashValue(hash, node.state.score.matchBrands);
            detail::hashValue(hash, node.state.dayBrands);
            for (AgentIndex a = 0; a < node.state.agentCount; ++a) {
                const Coord c = field.toCoord(projectedPosition(node.state, a));
                detail::hashValue(hash, c.x / 3);
                detail::hashValue(hash, c.y / 3);
                detail::hashValue(hash, node.aims[a]);
            }
            return hash;
        };
        std::unordered_map<std::size_t, bool> niches;
        auto take = [&](std::size_t i) {
            indices.push_back(i);
            chosen[i] = true;
            niches[signature(nodes[i])] = true;
        };
        for (std::size_t i = 0; i < nodes.size() && indices.size() < elite; ++i)
            if (!removed[i]) take(i);
        for (std::size_t i = 0; i < nodes.size() && indices.size() < limit; ++i)
            if (!removed[i] && !chosen[i] && niches.count(signature(nodes[i])) == 0)
                take(i);
        for (std::size_t i = 0; i < nodes.size() && indices.size() < limit; ++i)
            if (!removed[i] && !chosen[i]) take(i);
        std::sort(indices.begin(), indices.end());
        unique.clear();
        unique.reserve(indices.size());
        for (const auto i : indices) unique.push_back(std::move(nodes[i]));
        nodes.swap(unique);
        unique.clear();
    }

    // 各スポットへの (所要時間, 必要燃料) の非劣解を求める。
    // 時間最短経路と燃料最小経路が異なる場合も、両制約を同時に判定できる。
    static SpotDistanceTables buildSpotDistances(
        const MatchSetting& setting,
        const MatchState& matchState,
        const std::function<bool()>& stop,
        std::size_t threadCount,
        ParallelExecutor& executor,
        bool& stopped,
        std::size_t cacheEntries,
        bool& reused) {
        const Field& field = setting.field;
        const auto capacity = std::min<std::size_t>(8, cacheEntries);
        DistanceCacheKey key;
        SpotDistanceTables result;
        if (capacity > 0) {
            key = makeDistanceCacheKey(setting, matchState);
            if (restoreCachedDistances(key, capacity, result)) {
                reused = true;
                return result;
            }
        }
        result.travel.resize(field.cellCount());
        result.firstHop.resize(field.cellCount());
        std::atomic<std::size_t> nextSource{0};
        std::atomic<bool> distanceStopped{false};
        executor.run(threadCount, [&](std::size_t) {
            for (;;) {
                const std::size_t source = nextSource.fetch_add(1);
                if (source >= field.cellCount() || distanceStopped.load()) break;
                auto table = Brain0::dijkstra(
                    setting, matchState, static_cast<CellPos>(source), [&]() {
                        if (stop()) distanceStopped.store(true);
                        return distanceStopped.load();
                    });
                result.travel[source] = std::move(table.distances);
                result.firstHop[source] = std::move(table.firstHop);
            }
        });
        if (distanceStopped.load()) {
            stopped = true;
            return result;
        }
        result.steps.assign(
            field.spots.size(),
            std::vector<std::int32_t>(
                field.cellCount(), kImpassableCost));
        result.fuel = result.steps;
        result.pareto.assign(
            field.spots.size(),
            std::vector<std::vector<PathLabel>>(field.cellCount()));

        std::atomic<std::size_t> nextSpot{0};
        std::atomic<bool> aborted{false};

        // 複数回補給が必要な経路も保持できるよう、単純経路で必要になり得る
        // 燃料量まで非劣ラベルを作る。直接到達時は残燃料で絞る。
        const std::int32_t fuelCap = static_cast<std::int32_t>(
            std::min<std::int64_t>(kImpassableCost - 1,
                                  2LL * field.cellCount()));
        auto buildRow = [&](std::size_t spotIndex) {
            const CellPos goal = field.spots[spotIndex].pos;
            if (!field.contains(goal) || !field.at(goal).isPassable()) {
                return;
            }
            auto& rows = result.pareto[spotIndex];
            DialQueue<std::pair<std::int32_t, CellPos>> queue;
            rows[static_cast<std::size_t>(goal)].push_back(
                PathLabel{0, 0, kInvalidCell});
            queue.push(0, {0, goal});
            while (!queue.empty() && !aborted.load(std::memory_order_relaxed)) {
                if (stop()) {
                    aborted.store(true, std::memory_order_relaxed);
                    return;
                }
                const auto entry = queue.pop();
                const std::int32_t steps = entry.first;
                const std::int32_t fuel = entry.second.first;
                const CellPos to = entry.second.second;
                const auto& currentLabels =
                    rows[static_cast<std::size_t>(to)];
                const auto current = std::find_if(
                    currentLabels.begin(),
                    currentLabels.end(),
                    [steps, fuel](const PathLabel& label) {
                        return label.steps == steps && label.fuel == fuel;
                    });
                if (current == currentLabels.end()) {
                    continue;
                }
                for (const CellPos from : field.neighbors(to)) {
                    if (!field.contains(from) || !field.at(from).isPassable()) {
                        continue;
                    }
                    const std::int32_t edgeSteps =
                        moveStepCost(setting, matchState, from);
                    const std::int32_t edgeFuel = moveFuelCost(
                        setting, AgentKind::Patrol, from);
                    if (edgeSteps >= kImpassableCost ||
                        edgeFuel >= kImpassableCost ||
                        steps > kImpassableCost - edgeSteps ||
                        fuel > fuelCap - edgeFuel) {
                        continue;
                    }
                    PathLabel candidate{
                        steps + edgeSteps,
                        fuel + edgeFuel,
                        to,
                    };
                    auto& labels = rows[static_cast<std::size_t>(from)];
                    bool dominated = false;
                    for (PathLabel& label : labels) {
                        if (label.steps <= candidate.steps &&
                            label.fuel <= candidate.fuel) {
                            if (label.steps == candidate.steps &&
                                label.fuel == candidate.fuel &&
                                (label.next == kInvalidCell ||
                                 candidate.next < label.next)) {
                                label.next = candidate.next;
                            }
                            dominated = true;
                            break;
                        }
                    }
                    if (dominated) {
                        continue;
                    }
                    labels.erase(
                        std::remove_if(
                            labels.begin(),
                            labels.end(),
                            [&](const PathLabel& label) {
                                return candidate.steps <= label.steps &&
                                       candidate.fuel <= label.fuel;
                            }),
                        labels.end());
                    labels.push_back(candidate);
                    queue.push(candidate.steps, {candidate.fuel, from});
                }
            }

            for (std::size_t pos = 0; pos < rows.size(); ++pos) {
                result.steps[spotIndex][pos] = result.travel[pos][goal];
                for (const PathLabel& label : rows[pos]) {
                    result.fuel[spotIndex][pos] = std::min(
                        result.fuel[spotIndex][pos], label.fuel);
                }
            }
        };

        auto worker = [&](std::size_t) {
            while (!aborted.load(std::memory_order_relaxed)) {
                const std::size_t task =
                    nextSpot.fetch_add(1, std::memory_order_relaxed);
                if (task >= field.spots.size()) {
                    return;
                }
                buildRow(task);
            }
        };

        const std::size_t workers = std::max<std::size_t>(
            1,
            std::min(threadCount, field.spots.size()));
        executor.run(workers, worker);
        stopped = aborted.load(std::memory_order_relaxed);
        // 中断した前計算や、探索ごとのコールバック・割当状態は保存しない。
        if (!stopped && capacity > 0)
            storeCachedDistances(std::move(key), result, capacity);
        return result;
    }

    static const PathLabel* constrainedPath(
        const SpotDistanceTables& distances,
        std::size_t spotIndex,
        CellPos from,
        std::int32_t maxSteps,
        std::int32_t maxFuel) noexcept {
        if (spotIndex >= distances.pareto.size() || from < 0 ||
            static_cast<std::size_t>(from) >=
                distances.pareto[spotIndex].size()) {
            return nullptr;
        }
        if (distances.steps[spotIndex][from] > maxSteps ||
            distances.fuel[spotIndex][from] > maxFuel) return nullptr;
        const PathLabel* best = nullptr;
        for (const PathLabel& label : distances.pareto[spotIndex][
                 static_cast<std::size_t>(from)]) {
            if (label.steps > maxSteps || label.fuel > maxFuel) {
                continue;
            }
            if (best == nullptr || label.steps < best->steps ||
                (label.steps == best->steps && label.fuel < best->fuel) ||
                (label.steps == best->steps && label.fuel == best->fuel &&
                 label.next < best->next)) {
                best = &label;
            }
        }
        return best;
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


    using TargetAssignments = std::array<PatrolTarget, kMaxAgents>;
    using ForbiddenTargets = std::array<std::array<bool, kMaxSpots>, kMaxAgents>;

    static std::int32_t projectedFuel(
        const GameState& state, AgentIndex a) noexcept {
        return std::max(0, state.agents[a].fuel -
            (state.pendingActions[a].isMove()
                 ? state.pendingActions[a].fuelCost : 0));
    }

    static const PathLabel* suffixLabel(
        const SpotDistanceTables& dist, std::size_t spot, CellPos next,
        std::int32_t steps, std::int32_t fuel) noexcept {
        for (const auto& label : dist.pareto[spot][next]) {
            if (label.steps == steps && label.fuel == fuel) return &label;
        }
        return nullptr;
    }

    // 巡回車→スポット→系列の最小費用流。系列の初回だけ報酬を付ける。
    static TargetAssignments assignPatrolTargets(
        const GameState& input, const MatchSetting& setting,
        const MatchState& matchState, const SpotDistanceTables& dist,
        bool currentDay = true, const ForbiddenTargets* forbidden = nullptr,
        RendezvousMemo* memo = nullptr) {
        GameState state = input;
        // 現在の予約に基づく次ステップの取得を仮予約し、その先を割り当てる。
        // 他車のstep 1到着も添字順に反映する。実状態は変更しない。
        if (currentDay && state.remainingSteps() > 0) {
            const std::int32_t originalDayUdon = state.dayUdon;
            std::array<CellPos, kMaxAgents> originalPositions{};
            for (AgentIndex a = 0; a < state.agentCount; ++a) {
                originalPositions[a] = state.agents[a].pos;
                const auto& pending = state.pendingActions[a];
                if (pending.isMove() && pending.remainingSteps == 1)
                    state.agents[a].pos = pending.destination;
            }
            for (AgentIndex a = 0; a < state.agentCount; ++a) {
                const auto* spot = setting.field.spotAt(state.agents[a].pos);
                if (spot) state.tryAcquire(a, static_cast<std::size_t>(
                    setting.field.at(state.agents[a].pos).spotIndex), setting.field);
            }
            for (AgentIndex a = 0; a < state.agentCount; ++a)
                state.agents[a].pos = originalPositions[a];
            state.dayUdon = originalDayUdon;
        }
        TargetAssignments result{};
        const int n = static_cast<int>(state.agentCount);
        const int spots = static_cast<int>(state.spotCount);
        const int brands = static_cast<int>(setting.field.brands.size());
        const int spotBase = n + 1, brandBase = spotBase + spots;
        const int sink = brandBase + brands;
        // 巡回先用と補給割当用は別workspace。スレッドごとに保持し、
        // 探索状態間でネットワークの辺・最短路バッファだけを再利用する。
        static thread_local MinCostFlow flow(0);
        flow.reset(sink + 1);
        flow.reserveEdges(0, n);
        for (int a = 0; a < n; ++a) flow.reserveEdges(a + 1, spots + 2);
        for (int i = 0; i < spots; ++i) flow.reserveEdges(spotBase + i, n + 1);
        std::array<std::array<int, kMaxSpots>, kMaxAgents> edges{};
        std::array<std::array<std::int32_t, kMaxSpots>, kMaxAgents> times{};
        for (auto& row : edges) row.fill(-1);
        const std::int64_t timeBound = std::max<std::int64_t>(
            state.dayStepLimit, 4LL * setting.field.cellCount());
        // 全車分の時間・先読み誘導値の差より1玉の報酬を大きくする。
        const std::int64_t pickup = timeBound *
            (currentDay && dist.planning.twoSpotLookahead ? 2 * n + 1 : n + 1) + 1;
        const std::int64_t daily = pickup * (n + 1);
        const std::int64_t fresh = daily * (n + 1);
        for (int a = 0; a < n; ++a) {
            if (!state.agents[a].isPatrol()) continue;
            flow.addEdge(0, a + 1, 1, 0);
            flow.addEdge(a + 1, sink, 1, 0); // 未割当も許す。
            const CellPos from = projectedPosition(state, a);
            const int delay = projectedDelay(state, a);
            const int available = currentDay
                ? state.remainingSteps() - delay : kImpassableCost - 1;
            for (int i = 0; i < spots; ++i) {
                if (forbidden && (*forbidden)[a][i]) continue;
                if (currentDay && (state.remainingStocks[i] <= 0 ||
                                   state.hasAcquired(a, i))) continue;
                int travel = dist.steps[i][from];
                if (travel >= kImpassableCost || travel > available) continue;
                int arrivalFuel = 0;
                if (currentDay) {
                    const auto* direct = constrainedPath(
                        dist, i, from, available, projectedFuel(state, a));
                    if (direct) {
                        travel = direct->steps;
                        arrivalFuel = projectedFuel(state, a) - direct->fuel;
                    } else {
                        const PatrolTarget target{
                            static_cast<std::size_t>(i), 0, travel, true};
                        const auto meet = findRendezvous(
                            state, a, target, setting, matchState, dist, kMaxAgents, memo);
                        if (!meet.valid) continue;
                        travel = meet.goalArrival - delay;
                        const int at = meetingStep(
                            state, meet.patrolArrival, meet.supplyArrival);
                        const auto* suffix = constrainedPath(dist, i, meet.pos,
                            state.remainingSteps() - at, setting.fuelLimit);
                        if (suffix) arrivalFuel = setting.fuelLimit - suffix->fuel;
                    }
                }
                // 次の1店も時間・残燃料内で到達できる場合だけ先読みする。
                // 即時取得の辞書順を覆さない範囲で、巡回順序を誘導する。
                std::int64_t continuation = 0;
                if (currentDay && dist.planning.twoSpotLookahead) {
                    const int left = available - travel;
                    const auto firstBrand = setting.field.spots[i].brandIndex;
                    std::size_t checked = 0;
                    for (const auto next : dist.nextSpots[i]) {
                        const int j = static_cast<int>(next);
                        if (j == i || state.remainingStocks[j] <= 0 ||
                            state.hasAcquired(a, j)) continue;
                        if (checked++ >= dist.planning.lookaheadCandidateLimit) break;
                        if (dist.steps[j][setting.field.spots[i].pos] > left) break;
                        const auto* second = constrainedPath(dist, j,
                            setting.field.spots[i].pos, left, arrivalFuel);
                        if (!second) continue;
                        const auto b = setting.field.spots[j].brandIndex;
                        const auto bit = brandBit(b);
                        const int value = b != firstBrand &&
                            (state.score.matchBrands & bit) == 0 ? 3 :
                            (b != firstBrand && (state.dayBrands & bit) == 0 ? 2 : 1);
                        continuation = std::max(continuation,
                            std::min(timeBound / 2,
                                static_cast<std::int64_t>(value) *
                                (left - second->steps + 1)));
                    }
                }
                times[a][i] = travel;
                edges[a][i] = flow.addEdge(
                    a + 1, spotBase + i, 1, travel - pickup - continuation);
            }
        }
        for (int i = 0; i < spots; ++i) {
            const Spot& spot = setting.field.spots[i];
            flow.addEdge(spotBase + i, brandBase + spot.brandIndex,
                         currentDay ? state.remainingStocks[i] : spot.maxStock, 0);
        }
        for (int b = 0; b < brands; ++b) {
            const BrandMask bit = brandBit(b);
            const std::int64_t reward =
                ((state.score.matchBrands & bit) == 0 ? fresh : 0) +
                ((!currentDay || (state.dayBrands & bit) == 0) ? daily : 0);
            flow.addEdge(brandBase + b, sink, 1, -reward);
            flow.addEdge(brandBase + b, sink, n, 0);
        }
        {
            PlanningTimer timer(
                dist.planning.profilePlanning ? &dist.timings->flow : nullptr);
            flow.run(0, sink, n);
        }
        std::array<AgentIndex, kMaxBrands> owners{};
        owners.fill(kMaxAgents);
        for (int a = 0; a < n; ++a) {
            for (int i = 0; i < spots; ++i) {
                if (edges[a][i] < 0 || flow.capacity(a + 1, edges[a][i]) != 0)
                    continue;
                result[a] = {static_cast<std::size_t>(i), 2, times[a][i], true};
                const auto b = setting.field.spots[i].brandIndex;
                const auto owner = owners[b];
                if (owner == kMaxAgents ||
                    times[a][i] + projectedDelay(state, a) <
                    result[owner].steps + projectedDelay(state, owner))
                    owners[b] = static_cast<AgentIndex>(a);
            }
        }
        for (int b = 0; b < brands; ++b) {
            if (owners[b] == kMaxAgents) continue;
            const auto bit = brandBit(b);
            result[owners[b]].priority = (state.score.matchBrands & bit) == 0
                ? 0 : ((!currentDay || (state.dayBrands & bit) == 0) ? 1 : 2);
        }
        return result;
    }

    static bool shouldWaitAtSpot(
        const GameState& state, AgentIndex a, const MatchSetting& setting,
        const TargetAssignments& targets) noexcept {
        const CellPos pos = projectedPosition(state, a);
        return state.agents[a].isPatrol() && setting.field.contains(pos) &&
               setting.field.at(pos).hasSpot() && !targets[a].valid;
    }

    // step 0では補給されないため、同位置での合流もstep 1以降。
    static int meetingStep(const GameState& state, int patrol, int supply) {
        return std::max({patrol, supply, state.step == 0 ? 1 : 0});
    }

    // 非劣経路ごとの実際のprefixをたどり、補給車の実距離で合流を判定。
    static RendezvousPlan findRendezvous(
        const GameState& state, AgentIndex patrolIndex,
        const PatrolTarget& target, const MatchSetting& setting,
        const MatchState& matchState, const SpotDistanceTables& dist,
        AgentIndex onlySupply = kMaxAgents, RendezvousMemo* memo = nullptr) {
        if (memo && target.valid) {
            if (onlySupply == kMaxAgents && memo->soleSupply < kMaxAgents)
                onlySupply = memo->soleSupply;
            const auto key = (patrolIndex * kMaxSpots + target.spotIndex) *
                (kMaxAgents + 1) + onlySupply;
            const auto index = memo->indices[key];
            if (index != RendezvousMemo::missing) return memo->plans[index];
            const auto result = findRendezvous(state, patrolIndex, target,
                setting, matchState, dist, onlySupply);
            memo->indices[key] = static_cast<std::uint16_t>(memo->plans.size());
            memo->plans.push_back(result);
            return result;
        }
        PlanningTimer timer(
            dist.planning.profilePlanning ? &dist.timings->rendezvous : nullptr);
        RendezvousPlan best;
        if (!target.valid || !state.agents[patrolIndex].isPatrol()) return best;
        const CellPos start = projectedPosition(state, patrolIndex);
        const int fuel = projectedFuel(state, patrolIndex);
        const int delay = projectedDelay(state, patrolIndex);
        const int available = state.remainingSteps();
        const auto* direct = constrainedPath(
            dist, target.spotIndex, start, available - delay, fuel);
        best.required = direct == nullptr;
        const int need = dist.fuel[target.spotIndex][start];
        const int arrivalFuel = direct != nullptr
            ? std::max(0, fuel - direct->fuel)
            : fuel;
        best.fuelShortage = best.required
            ? std::max(1, need < kImpassableCost ? need - fuel : 1)
            : std::max(
                  std::max(0, (setting.fuelLimit + 1) / 2 - fuel),
                  arrivalFuel == 0 ? 1 : 0);
        if (!best.required && best.fuelShortage == 0) return best;
        const CellPos goal = setting.field.spots[target.spotIndex].pos;
        for (const PathLabel& route : dist.pareto[target.spotIndex][start]) {
            if (route.steps + delay > available) continue;
            const PathLabel* label = &route;
            CellPos cell = start;
            int arrival = delay, left = fuel;
            for (std::size_t guard = 0; guard <= setting.field.cellCount(); ++guard) {
                for (AgentIndex q = 0; q < state.agentCount; ++q) {
                    if (!state.agents[q].isSupply() ||
                        (onlySupply < kMaxAgents && q != onlySupply)) continue;
                    const int travel = dist.travel[projectedPosition(state, q)][cell];
                    if (travel >= kImpassableCost) continue;
                    const int supplyArrival = projectedDelay(state, q) + travel;
                    const int meetAt = meetingStep(state, arrival, supplyArrival);
                    const auto* suffix = constrainedPath(
                        dist, target.spotIndex, cell,
                        available - meetAt, setting.fuelLimit);
                    const bool repeated = suffix == nullptr;
                    if (repeated &&
                        (label == nullptr ||
                         label->steps > available - meetAt)) {
                        continue;
                    }
                    const int goalArrival = meetAt +
                        (suffix != nullptr ? suffix->steps : label->steps);
                    // 直接到達できる車を、任意の補給で遅らせない。
                    if (direct != nullptr &&
                        goalArrival > delay + direct->steps) continue;
                    const int wait = meetAt - arrival;
                    const int bestWait = best.valid
                        ? meetingStep(state, best.patrolArrival, best.supplyArrival)
                            - best.patrolArrival
                        : kImpassableCost;
                    if (!best.valid ||
                        std::make_tuple(
                            goalArrival, repeated, wait, -arrival,
                            travel, q, cell) <
                        std::make_tuple(
                            best.goalArrival,
                            best.needsRefuelAgain,
                            bestWait,
                            -best.patrolArrival,
                            best.supplyTravel,
                            best.supply,
                            best.pos)) {
                        best.pos = cell;
                        best.supply = q;
                        best.patrolArrival = arrival;
                        best.supplyArrival = supplyArrival;
                        best.supplyTravel = travel;
                        best.goalArrival = goalArrival;
                        best.path = route;
                        best.needsRefuelAgain = repeated;
                        best.valid = true;
                    }
                }
                if (cell == goal || label == nullptr) break;
                const int edgeFuel = moveFuelCost(setting, AgentKind::Patrol, cell);
                const int edgeSteps = moveStepCost(setting, matchState, cell);
                if (edgeFuel > left || edgeSteps >= kImpassableCost) break;
                const CellPos next = label->next;
                if (!setting.field.contains(next)) break;
                label = suffixLabel(dist, target.spotIndex, next,
                                    label->steps - edgeSteps, label->fuel - edgeFuel);
                if (!label) break;
                left -= edgeFuel;
                arrival += edgeSteps;
                cell = next;
            }
        }
        // 巡回車だけの時間・燃料では劣る迂回でも、補給車に近づくことで
        // 合流込みでは早くなる。到着時刻の下界で候補を絞って実経路を確認。
        // 合流が見つかっていても、待ち時間や燃料節約経路の迂回を
        // 短縮できる余地があれば、経路外の合流地点も比較する。
        const int earliestGoal = delay + dist.steps[target.spotIndex][start];
        if (best.required && (!best.valid || best.goalArrival > earliestGoal ||
                              best.needsRefuelAgain) && dist.planning.detourRendezvous &&
            dist.planning.detourCandidateLimit > 0 && fuel > 0) {
            const bool refineExisting = best.valid;
            using Candidate = std::tuple<int, int, AgentIndex, CellPos>;
            std::vector<Candidate> candidates;
            for (AgentIndex q = 0; q < state.agentCount; ++q) {
                if (!state.agents[q].isSupply() ||
                    (onlySupply < kMaxAgents && q != onlySupply)) continue;
                for (CellPos cell = 0;
                     cell < static_cast<CellPos>(setting.field.cellCount()); ++cell) {
                    const int patrol = dist.travel[start][cell];
                    // 全地形・交通で移動時間 <= 2 * 消費燃料。
                    // 最短時間さえ2*fuelを超える地点には補給前に届かない。
                    if (patrol > 2LL * fuel) continue;
                    const int supply = dist.travel[projectedPosition(state, q)][cell];
                    if (patrol >= kImpassableCost || supply >= kImpassableCost)
                        continue;
                    const int at = meetingStep(
                        state, delay + patrol, projectedDelay(state, q) + supply);
                    if (dist.fuel[target.spotIndex][cell] > setting.fuelLimit) continue;
                    const int suffixSteps = dist.steps[target.spotIndex][cell];
                    if (suffixSteps > available - at) continue;
                    const int lower = at + suffixSteps;
                    if (best.valid && (lower > best.goalArrival ||
                        (lower == best.goalArrival && !best.needsRefuelAgain))) continue;
                    candidates.emplace_back(lower, supply, q, cell);
                }
            }
            const auto retained = std::min(
                candidates.size(), dist.planning.detourCandidateLimit);
            std::partial_sort(candidates.begin(), candidates.begin() +
                static_cast<std::ptrdiff_t>(retained), candidates.end());
            std::size_t checked = 0;
            for (const auto& entry : candidates) {
                if (checked++ >= dist.planning.detourCandidateLimit ||
                    (dist.stop && dist.stop())) break;
                const int lower = std::get<0>(entry);
                if (best.valid && (lower > best.goalArrival ||
                    (lower == best.goalArrival && !best.needsRefuelAgain))) break;
                const auto q = std::get<2>(entry);
                const auto cell = std::get<3>(entry);
                AgentPlan actions;
                int prefixSteps = 0;
                if (refineExisting) {
                    // 追加改善の試行は事前計算済み最短路の復元だけに限定。
                    // 既存の合流がない場合の燃料節約経路探索は従来どおり行う。
                    actions = routeTo(setting, matchState, dist, start, cell,
                                      AgentKind::Patrol, available - delay, fuel);
                    CellPos reached = start;
                    for (const int action : actions) {
                        prefixSteps += moveStepCost(setting, matchState, reached);
                        reached = setting.field.neighbor(reached, static_cast<Direction>(action));
                    }
                    if (reached != cell) continue;
                } else {
                    const auto prefix = Brain0::shortestFuelConstrainedPath(
                        setting, matchState, start, cell, AgentKind::Patrol,
                        fuel, dist.stop);
                    if (!prefix.reachable) continue;
                    prefixSteps = prefix.cost.steps;
                    for (const auto direction : prefix.directions)
                        actions.push_back(static_cast<int>(direction));
                }
                const int arrival = delay + prefixSteps;
                const int supply = std::get<1>(entry);
                const int supplyArrival = projectedDelay(state, q) + supply;
                const int at = meetingStep(state, arrival, supplyArrival);
                const auto* suffix = constrainedPath(dist, target.spotIndex,
                    cell, available - at, setting.fuelLimit);
                if (!suffix) continue;
                const int goalArrival = at + suffix->steps;
                if (best.valid && (goalArrival > best.goalArrival ||
                    (goalArrival == best.goalArrival && !best.needsRefuelAgain))) continue;
                best.pos = cell;
                best.supply = q;
                best.patrolArrival = arrival;
                best.supplyArrival = supplyArrival;
                best.supplyTravel = supply;
                best.goalArrival = goalArrival;
                best.detourPrefix = std::make_shared<const AgentPlan>(std::move(actions));
                best.needsRefuelAgain = false;
                best.valid = true;
            }
        }
        return best;
    }

    using RendezvousAssignments =
        std::array<RendezvousPlan, kMaxAgents>;

    // 組合せ全体で補給要求を最大化する一対一最小費用割当。
    static RendezvousAssignments assignRendezvous(
        const GameState& state, const MatchSetting& setting,
        const MatchState& matchState, const SpotDistanceTables& dist,
        const TargetAssignments& targets, RendezvousMemo* memo = nullptr) {
        RendezvousAssignments result{};
        const int n = static_cast<int>(state.agentCount), sink = 2 * n + 1;
        static thread_local MinCostFlow flow(0);
        flow.reset(sink + 1);
        for (int vertex = 0; vertex <= sink; ++vertex) flow.reserveEdges(vertex, n + 2);
        std::array<std::array<RendezvousPlan, kMaxAgents>, kMaxAgents> pairs{};
        std::array<std::array<int, kMaxAgents>, kMaxAgents> edges{};
        for (auto& row : edges) row.fill(-1);
        const std::int64_t unit =
            (static_cast<std::int64_t>(state.dayStepLimit) + 1) * (n + 1) * 8;
        const std::int64_t benefit = unit * (setting.fuelLimit + 1LL) * (n + 1);
        for (int q = 0; q < n; ++q)
            if (state.agents[q].isSupply()) flow.addEdge(n + 1 + q, sink, 1, 0);
        for (int p = 0; p < n; ++p) {
            if (!state.agents[p].isPatrol()) continue;
            flow.addEdge(0, p + 1, 1, 0);
            flow.addEdge(p + 1, sink, 1, 0);
            const CellPos pos = projectedPosition(state, p);
            const int fuel = projectedFuel(state, p);
            const CellPos standbyGoal = targets[p].valid
                ? setting.field.spots[targets[p].spotIndex].pos
                : kInvalidCell;
            const int standbyDelay = projectedDelay(state, p);
            const auto* standbyDirect = targets[p].valid
                ? constrainedPath(
                      dist, targets[p].spotIndex, pos,
                      state.remainingSteps() - standbyDelay, fuel)
                : nullptr;
            const int standbyPatrolArrival = standbyDirect != nullptr
                ? standbyDelay + standbyDirect->steps
                : kImpassableCost;
            int standbyFuelShortage = 0;
            if (standbyDirect != nullptr) {
                const int arrivalFuel = std::max(0, fuel - standbyDirect->fuel);
                int nextPriority = 3;
                int nextFuel = kImpassableCost;
                const int meetAt = meetingStep(
                    state, standbyPatrolArrival, standbyPatrolArrival);
                for (std::size_t i = 0; i < state.spotCount; ++i) {
                    if (i == targets[p].spotIndex ||
                        state.remainingStocks[i] <= 0 ||
                        state.hasAcquired(p, i)) {
                        continue;
                    }
                    const int requiredFuel = dist.fuel[i][standbyGoal];
                    const int requiredSteps = dist.steps[i][standbyGoal];
                    if (requiredFuel > setting.fuelLimit ||
                        requiredSteps > state.remainingSteps() - meetAt) {
                        continue;
                    }
                    const auto bit = brandBit(setting.field.spots[i].brandIndex);
                    const int priority =
                        (state.score.matchBrands & bit) == 0 ? 0 :
                        ((state.dayBrands & bit) == 0 ? 1 : 2);
                    if (std::make_pair(priority, requiredFuel) <
                        std::make_pair(nextPriority, nextFuel)) {
                        nextPriority = priority;
                        nextFuel = requiredFuel;
                    }
                }
                if (nextFuel < kImpassableCost) {
                    standbyFuelShortage = std::max(0, nextFuel - arrivalFuel);
                }
            }
            for (int q = 0; q < n; ++q) {
                if (!state.agents[q].isSupply()) continue;
                auto& meet = pairs[p][q];
                if (targets[p].valid) {
                    meet = findRendezvous(
                        state, p, targets[p], setting, matchState, dist, q, memo);
                    // 緊急の補給点がなくても、補給車が巡回車の次の
                    // スポットへ先着できるなら待機地点として割り当てる。
                    // 巡回車の到着を遅らせず、一対一割当の中で競合も避ける。
                    if (!meet.valid) {
                        const int travel = dist.travel[
                            projectedPosition(state, q)][standbyGoal];
                        const int supplyArrival = projectedDelay(state, q) + travel;
                        const int meetAt = meetingStep(
                            state, standbyPatrolArrival, supplyArrival);
                        if (standbyDirect != nullptr && travel < kImpassableCost &&
                            supplyArrival <= standbyPatrolArrival &&
                            meetAt <= state.remainingSteps()) {
                            meet.pos = standbyGoal;
                            meet.supply = q;
                            meet.patrolArrival = standbyPatrolArrival;
                            meet.supplyArrival = supplyArrival;
                            meet.supplyTravel = travel;
                            meet.fuelShortage = standbyFuelShortage;
                            meet.goalArrival = meetAt;
                            meet.path = *standbyDirect;
                            meet.valid = true;
                        }
                    }
                } else if (setting.field.at(pos).hasSpot() && fuel < setting.fuelLimit) {
                    const int travel = dist.travel[projectedPosition(state, q)][pos];
                    const int arrival = projectedDelay(state, q) + travel;
                    if (travel < kImpassableCost &&
                        meetingStep(state, projectedDelay(state, p), arrival)
                            <= state.remainingSteps()) {
                        meet.pos = pos;
                        meet.supply = q;
                        meet.patrolArrival = projectedDelay(state, p);
                        meet.supplyArrival = arrival;
                        meet.supplyTravel = travel;
                        meet.fuelShortage = setting.fuelLimit - fuel;
                        meet.goalArrival = std::max(arrival, meet.patrolArrival);
                        meet.valid = true;
                    }
                }
                if (!meet.valid) continue;
                const int wait = std::max(0, meet.supplyArrival - meet.patrolArrival);
                const std::int64_t reward = benefit +
                    (meet.required ? benefit * (n + 1) : 0) +
                    (meet.required && targets[p].valid
                         ? benefit * (2 - targets[p].priority) : 0) +
                    unit * meet.fuelShortage;
                edges[p][q] = flow.addEdge(
                    p + 1, n + 1 + q, 1,
                    4LL * meet.goalArrival + 4LL * wait +
                        meet.supplyTravel - reward);
            }
        }
        {
            PlanningTimer timer(
                dist.planning.profilePlanning ? &dist.timings->flow : nullptr);
            flow.run(0, sink, n);
        }
        for (int p = 0; p < n; ++p)
            for (int q = 0; q < n; ++q)
                if (edges[p][q] >= 0 && flow.capacity(p + 1, edges[p][q]) == 0)
                    result[p] = pairs[p][q];
        return result;
    }

    struct Assignments {
        TargetAssignments targets{};
        RendezvousAssignments meets{};
    };

    struct AssignmentCache {
        std::mutex mutex;
        std::unordered_map<GameState, Assignments, GameStateHash, GameStateEqual> entries;
        // ParallelExecutor が複数の探索ノードを同時評価するときだけ
        // entries の同期が必要。単一スレッド探索では、非常に高頻度な
        // キャッシュ参照ごとの mutex 取得を避ける。
        bool synchronized = true;
        std::atomic<std::size_t> hits{0};
        std::atomic<std::size_t> computations{0};
        std::atomic<std::size_t> repairs{0};
        std::atomic<std::size_t> detours{0};
    };

    static Assignments coordinatedAssignments(
        const GameState& state, const MatchSetting& setting,
        const MatchState& matchState, const SpotDistanceTables& dist) {
        const auto key = canonicalSearchState(state);
        auto& cache = *dist.assignments;
        if (dist.planning.assignmentCacheLimit > 0) {
            if (cache.synchronized) {
                std::lock_guard<std::mutex> lock(cache.mutex);
                const auto found = cache.entries.find(key);
                if (found != cache.entries.end()) {
                    ++cache.hits;
                    return found->second;
                }
            } else {
                const auto found = cache.entries.find(key);
                if (found != cache.entries.end()) {
                    ++cache.hits;
                    return found->second;
                }
            }
        }
        ++cache.computations;
        PlanningTimer timer(
            dist.planning.profilePlanning ? &dist.timings->assignment : nullptr);
        Assignments result;
        static thread_local RendezvousMemo memo;
        memo.reset(state);
        ForbiddenTargets forbidden{};
        const bool repairUnreachable =
            dist.planning.coordinatedAssignments &&
            !setting.daySteps.empty() &&
            setting.fuelLimit <= setting.daySteps.front();
        for (std::size_t round = 0; round <= state.agentCount; ++round) {
            result.targets = assignPatrolTargets(
                state, setting, matchState, dist, true, &forbidden, &memo);
            result.meets = assignRendezvous(
                state, setting, matchState, dist, result.targets, &memo);
            if (!repairUnreachable) break;
            bool retry = false;
            for (AgentIndex a = 0; a < state.agentCount; ++a) {
                const auto target = result.targets[a];
                if (!target.valid) continue;
                const auto* direct = constrainedPath(dist, target.spotIndex,
                    projectedPosition(state, a),
                    state.remainingSteps() - projectedDelay(state, a),
                    projectedFuel(state, a));
                if (direct || result.meets[a].valid) continue;
                forbidden[a][target.spotIndex] = true;
                ++cache.repairs;
                result.targets[a] = PatrolTarget{};
                retry = true;
            }
            if (!retry || (dist.stop && dist.stop())) break;
        }
        for (const auto& meet : result.meets)
            if (meet.detourPrefix) ++cache.detours;
        if (dist.planning.assignmentCacheLimit > 0) {
            auto store = [&]() {
                if (cache.entries.size() >= dist.planning.assignmentCacheLimit)
                    cache.entries.clear();
                cache.entries.emplace(key, result);
            };
            if (cache.synchronized) {
                std::lock_guard<std::mutex> lock(cache.mutex);
                store();
            } else {
                store();
            }
        }
        return result;
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
        const auto assignment = coordinatedAssignments(state, setting, matchState, spotDistances);
        const auto& assignedTargets = assignment.targets;
        const auto& assignedRendezvous = assignment.meets;

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
            std::int32_t projectedFuel = agent.fuel;
            if (state.pendingActions[agentIndex].isMove()) {
                projectedFuel -=
                    state.pendingActions[agentIndex].fuelCost;
            }
            projectedFuel = std::max(0, projectedFuel);
            const PatrolTarget target = assignedTargets[agentIndex];
            const bool waitsAtSpot = shouldWaitAtSpot(
                state, agentIndex, setting, assignedTargets);
            if (target.valid) {
                targets[agentIndex] =
                    static_cast<std::int32_t>(target.spotIndex);
            }
            const RendezvousPlan rendezvous =
                assignedRendezvous[agentIndex];
            const PathLabel* directPath = target.valid
                ? constrainedPath(
                    spotDistances,
                    target.spotIndex,
                    projected,
                    availableSteps,
                    projectedFuel)
                : nullptr;
            const bool directTarget = directPath != nullptr;

            // 全体割当された目標だけを距離評価する。同系列の別スポットへ
            // 複数台が向かうことを、距離評価側でも重複加点しない。
            if (target.valid && (directTarget || rendezvous.valid)) {
                const std::int32_t total = directTarget
                    ? reservedSteps + directPath->steps
                    : rendezvous.goalArrival;
                nearestSpot = total;
                if (target.priority == 0) {
                    nearestNew = total;
                }
                if (target.priority <= 1) {
                    nearestDaily = total;
                }
            }
            // 距離そのものを罰するのではなく「残り時間に対する進捗」を
            // 負値で保持する。到達不能(0点)が遠い到達可能状態より高く
            // 評価される逆転を防ぐ。
            const std::int32_t progressBase = state.remainingSteps() + 1;
            if (nearestSpot < kImpassableCost) {
                nearestSpot -= progressBase;
            }
            if (nearestNew < kImpassableCost) {
                nearestNew -= progressBase;
            }
            if (nearestDaily < kImpassableCost) {
                nearestDaily -= progressBase;
            }
            context.nearestValuableSpotSteps[agentIndex] = nearestSpot;
            context.nearestNewBrandSpotSteps[agentIndex] = nearestNew;
            context.nearestDailyBrandSpotSteps[agentIndex] = nearestDaily;
            if (target.valid && (directTarget || rendezvous.valid)) {
                context.targetSlack += std::max(
                    0, state.remainingSteps() -
                        (directTarget ? reservedSteps + directPath->steps
                                      : rendezvous.goalArrival));
            } else if (!waitsAtSpot) {
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
                    if (distance < kImpassableCost &&
                        distance <= availableSteps) {
                        hasTimeReachableCandidate = true;
                        break;
                    }
                }
                if (hasCandidate) {
                    ++context.unreachableTargetCount;
                    if (hasTimeReachableCandidate) {
                        context.fuelRisk += std::max(
                            1,
                            (setting.fuelLimit + 1) / 2 - projectedFuel);
                    }
                }
            }

            const std::int32_t fuelThreshold = std::max(
                1, (setting.fuelLimit + 1) / 2);
            const std::int32_t fuelNeed =
                std::max(0, fuelThreshold - projectedFuel);
            if (rendezvous.valid &&
                (rendezvous.required || rendezvous.fuelShortage > 0)) {
                context.nearestSupplySteps[agentIndex] =
                    rendezvous.supplyTravel;
                context.rendezvousMismatch += std::max(
                    0,
                    rendezvous.supplyArrival - rendezvous.patrolArrival);
                context.fuelRisk += std::max(
                    fuelNeed, rendezvous.fuelShortage);
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

        // スポット上で中間日を終えると、翌日のstep 1で在庫回復後の
        // うどんを確定取得できる。終端直前だけ評価して当日の巡回を
        // 早過ぎる段階で止めない。
        if (static_cast<std::size_t>(state.day + 1) <
                setting.dayCount() &&
            state.remainingSteps() <= 4) {
            std::array<std::int32_t, kMaxSpots> uses{};
            BrandMask nextBrands = 0;
            BrandMask nextNewBrands = 0;
            for (AgentIndex agentIndex = 0;
                 agentIndex < state.agentCount;
                 ++agentIndex) {
                if (!state.agents[agentIndex].isPatrol()) {
                    continue;
                }
                const CellPos pos =
                    projectedPosition(state, agentIndex);
                if (!setting.field.contains(pos) ||
                    !setting.field.at(pos).hasSpot()) {
                    continue;
                }
                const std::size_t spotIndex = static_cast<std::size_t>(
                    setting.field.at(pos).spotIndex);
                const Spot& spot = setting.field.spots[spotIndex];
                if (uses[spotIndex] >= spot.maxStock) {
                    continue;
                }
                ++uses[spotIndex];
                ++context.nextDayUdonCount;
                const BrandMask bit = brandBit(
                    static_cast<std::size_t>(spot.brandIndex));
                nextBrands |= bit;
                if ((state.score.matchBrands & bit) == 0) {
                    nextNewBrands |= bit;
                }
            }
            context.nextDayBrandCount = static_cast<std::int32_t>(
                countBits(nextBrands));
            context.nextDayNewBrandCount = static_cast<std::int32_t>(
                countBits(nextNewBrands));

            const TargetAssignments futureTargets = assignPatrolTargets(
                state, setting, matchState, spotDistances, false);
            const std::int32_t approachBase = static_cast<std::int32_t>(
                std::min<std::size_t>(
                    static_cast<std::size_t>(kImpassableCost - 1),
                    setting.field.cellCount() * 4 + 1));
            for (AgentIndex agentIndex = 0;
                 agentIndex < state.agentCount;
                 ++agentIndex) {
                const PatrolTarget& target = futureTargets[agentIndex];
                if (!target.valid || target.steps >= approachBase) {
                    continue;
                }
                const std::int32_t progress = approachBase - target.steps;
                if (target.priority == 0) {
                    context.nextDayNewBrandApproach += progress;
                }
                if (target.priority <= 1) {
                    context.nextDayBrandApproach += progress;
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
        const BeamSearchConfig& config,
        std::int64_t budgetMs) noexcept {
        EvaluationWeights weights = config.weights;
        if (!config.autoTuneWeights) {
            return weights;
        }

        if (config.fuelAwareWeights && budgetMs > 0 && budgetMs <= 300 &&
            setting.fuelLimit < setting.daySteps.front()) {
            weights.remainingFuel /= 1.5;
            weights.newBrandSpotDistance *= 1.5;
            weights.nextDayUdon /= 1.5;
            weights.nextDayBrandApproach /= 1.5;
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

    static std::int32_t directionTo(const Field& field, CellPos from, CellPos to) {
        for (std::int32_t d = 0; d < 6; ++d)
            if (field.neighbor(from, static_cast<Direction>(d)) == to) return d;
        return -1;
    }

    static AgentPlan routeTo(
        const MatchSetting& s, const MatchState& ms, const SpotDistanceTables& dist,
        CellPos start, CellPos goal, AgentKind kind, int time, int fuel,
        std::size_t spot = kMaxSpots, const PathLabel* label = nullptr) {
        AgentPlan route;
        CellPos cell = start;
        for (std::size_t guard = 0; cell != goal && guard < s.field.cellCount(); ++guard) {
            const int steps = moveStepCost(s, ms, cell);
            const int cost = moveFuelCost(s, kind, cell);
            if (steps > time || cost > fuel) break;
            const CellPos next = label ? label->next : dist.firstHop[cell][goal];
            if (!s.field.contains(next)) break;
            const int action = directionTo(s.field, cell, next);
            if (action < 0) break;
            const PathLabel* nextLabel = nullptr;
            if (label) {
                nextLabel = suffixLabel(dist, spot, next,
                    label->steps - steps, label->fuel - cost);
                if (!nextLabel) break;
            }
            route.push_back(action);
            time -= steps;
            fuel -= cost;
            cell = next;
            label = nextLabel;
        }
        return route;
    }

    static std::vector<AgentPlan> macroPlans(
        const GameState& state, AgentIndex a, const MatchSetting& s,
        const MatchState& ms, const SpotDistanceTables& dist,
        const TargetAssignments& targets, const RendezvousAssignments& meets,
        const BeamSearchConfig& cfg) {
        std::vector<AgentPlan> plans;
        const Agent& agent = state.agents[a];
        const int available = state.remainingSteps();
        auto add = [&](AgentPlan route) {
            if (!route.empty() &&
                std::find(plans.begin(), plans.end(), route) == plans.end())
                plans.push_back(std::move(route));
        };
        if (agent.isPatrol()) {
            std::vector<std::tuple<int, int, std::size_t>> spots;
            const bool future = static_cast<std::size_t>(state.day + 1) < s.dayCount();
            for (std::size_t i = 0; i < state.spotCount; ++i) {
                if ((!future && state.hasAcquired(a, i)) ||
                    (!future && state.remainingStocks[i] == 0)) continue;
                const auto bit = brandBit(s.field.spots[i].brandIndex);
                int priority = (state.score.matchBrands & bit) == 0 ? 0 :
                    ((state.dayBrands & bit) == 0 ? 1 : 2);
                if (targets[a].valid && targets[a].spotIndex == i) priority = -1;
                spots.emplace_back(priority, dist.steps[i][agent.pos], i);
            }
            std::sort(spots.begin(), spots.end());
            std::size_t used = 0;
            for (const auto& entry : spots) {
                if (used >= cfg.macroTargetLimit) break;
                const auto i = std::get<2>(entry);
                const auto* path = constrainedPath(
                    dist, i, agent.pos, available, agent.fuel);
                if (!path && future)
                    path = constrainedPath(dist, i, agent.pos,
                                           kImpassableCost - 1, agent.fuel);
                if (!path) continue;
                add(routeTo(s, ms, dist, agent.pos, s.field.spots[i].pos,
                            agent.kind, available, agent.fuel, i, path));
                ++used;
            }
            const auto& meet = meets[a];
            if (meet.valid && targets[a].valid) {
                const auto i = targets[a].spotIndex;
                auto route = meet.detourPrefix ? *meet.detourPrefix :
                    routeTo(s, ms, dist, agent.pos, meet.pos,
                        agent.kind, available, agent.fuel, i, &meet.path);
                CellPos reached = agent.pos;
                int elapsed = 0;
                for (const int action : route) {
                    elapsed += moveStepCost(s, ms, reached);
                    reached = s.field.neighbor(reached, static_cast<Direction>(action));
                }
                if (reached == meet.pos) {
                    const int at = meetingStep(state, elapsed, meet.supplyArrival);
                    if (at > elapsed) route.push_back(-(at - elapsed));
                    const auto* suffix = constrainedPath(dist, i, reached,
                        available - at, s.fuelLimit);
                    if (suffix) {
                        const auto rest = routeTo(s, ms, dist, reached,
                            s.field.spots[i].pos, agent.kind,
                            available - at, s.fuelLimit, i, suffix);
                        route.insert(route.end(), rest.begin(), rest.end());
                        add(std::move(route));
                    } else if (meet.needsRefuelAgain) {
                        // この合流で満タンにした後も目的地まで一充填では
                        // 届かない。ここでマクロを区切り、次のイベントで
                        // 新しい合流点を計算する。
                        if (route.empty() && available > 0) {
                            route.push_back(-1);
                        }
                        add(std::move(route));
                    }
                }
            }
        } else {
            for (const auto& meet : meets) {
                if (!meet.valid || meet.supply != a) continue;
                auto route = routeTo(s, ms, dist, agent.pos, meet.pos,
                                    agent.kind, available, s.fuelLimit);
                const int travel = dist.travel[agent.pos][meet.pos];
                const int wait = std::max({0, meet.patrolArrival - travel,
                                           (state.step == 0 ? 1 : 0) - travel});
                if (wait > 0 && travel + wait <= available) route.push_back(-wait);
                add(std::move(route));
            }
        }
        return plans;
    }

    static double scheduledActionBonus(
        const GameState& state,
        AgentIndex agentIndex,
        const MatchSetting& setting,
        const SpotDistanceTables& spotDistances,
        const EvaluationWeights& weights,
        const TargetAssignments& targets,
        const RendezvousAssignments& rendezvousAssignments) noexcept {
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
                const RendezvousPlan& rendezvous =
                    rendezvousAssignments[patrolIndex];
                if (!rendezvous.valid ||
                    rendezvous.supply != agentIndex) {
                    continue;
                }
                const int travel = spotDistances.travel[
                    projectedPosition(state, agentIndex)][rendezvous.pos];
                if (travel >= kImpassableCost) continue;
                const int wait = std::max(0, projectedDelay(state, agentIndex) +
                    travel - rendezvous.patrolArrival);
                const double urgency = rendezvous.required
                    ? -weights.fuelRisk * 2.0
                    : -weights.fuelRisk * 0.25;
                bestGuidance = std::max(
                    bestGuidance,
                    urgency +
                        weights.supplyDistance *
                            travel +
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
        const PatrolTarget& target = targets[agentIndex];
        if (target.valid) {
            const auto* path = constrainedPath(spotDistances, target.spotIndex,
                projected, state.remainingSteps() - pending.remainingSteps,
                projectedFuel(state, agentIndex));
            nearest = path ? path->steps :
                spotDistances.steps[target.spotIndex][projected];
            priority = target.priority;
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
        if (projected != agent.pos && destination.hasSpot()) {
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
        BeamSearchResult routeBest;
        const auto officialScore = [](const GameState& state) {
            return std::make_tuple(countBits(state.score.matchBrands),
                state.score.sumOfDailyBrandCounts, state.score.totalUdon);
        };
        auto cache = std::make_shared<AssignmentCache>();
        auto timings = std::make_shared<PlanningTimings>();
        std::atomic<std::size_t> fullEvals{0};
        std::atomic<std::size_t> reusedEvals{0};
        auto finish = [&](SearchStopReason reason) {
            // 経路完了案は従来の途中解を上書きせず別に保持する。
            // 最後に当日の確定得点と翌日を含む評価の両方で改善を確認する。
            if (routeBest.success && (!res.success ||
                (officialScore(routeBest.finalState) > officialScore(res.finalState) &&
                 routeBest.evaluation.total >= res.evaluation.total))) {
                res.success = true;
                res.plan = std::move(routeBest.plan);
                res.finalState = routeBest.finalState;
                res.evaluation = routeBest.evaluation;
            }
            if (res.success) {
                res.plan = compressWaitActions(res.plan);
            }
            res.stopReason = reason;
            res.elapsedMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(Clock::now() - started).count();
            res.fullEvaluations =
                fullEvals.load(std::memory_order_relaxed);
            res.reusedEvaluations =
                reusedEvals.load(std::memory_order_relaxed);
            res.assignmentCacheHits = cache->hits.load();
            res.assignmentComputations = cache->computations.load();
            res.repairedAssignments = cache->repairs.load();
            res.detourPlans = cache->detours.load();
            res.assignmentTimeNs = timings->assignment.load();
            res.flowTimeNs = timings->flow.load();
            res.rendezvousTimeNs = timings->rendezvous.load();
            return res;
        };

        if (cfg.beamWidth == 0 || cfg.jointBeamWidth == 0 ||
            cfg.timeLimitMs < 0 || cfg.safetyMarginMs < 0 ||
            cfg.incumbentInterval == 0 || !std::isfinite(cfg.diversityFraction) ||
            cfg.diversityFraction < 0 || cfg.diversityFraction > 1) {
            return finish(SearchStopReason::NoValidPlan);
        }

        GameState init;
        if (!init.initialize(s, ms, prev) || init.agentCount == 0 ||
            init.dayStepLimit <= 0) {
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
        cache->synchronized = threadCount > 1;

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

        const EvaluationWeights weights = tunedWeights(s, ms, cfg, budgetMs);
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
        ActionRoutes routes;
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
        SpotDistanceTables spotDist = buildSpotDistances(
            s,
            ms,
            [&]() { return expired(); },
            threadCount,
            executor,
            prepStopped,
            cfg.distanceCacheEntries,
            res.reusedDistanceTables);
        spotDist.planning = cfg;
        spotDist.assignments = cache;
        spotDist.timings = timings;
        spotDist.stop = [&]() { return expired(); };
        if (prepStopped) {
            return finish(
                hasBest ? SearchStopReason::TimeLimit
                        : SearchStopReason::NoValidPlan);
        }
        spotDist.nextSpots.resize(s.field.spots.size());
        for (std::size_t i = 0; i < s.field.spots.size(); ++i) {
            auto& next = spotDist.nextSpots[i];
            for (std::size_t j = 0; j < s.field.spots.size(); ++j)
                if (j != i) next.push_back(j);
            std::sort(next.begin(), next.end(), [&](std::size_t lhs, std::size_t rhs) {
                return std::make_pair(spotDist.steps[lhs][s.field.spots[i].pos], lhs) <
                       std::make_pair(spotDist.steps[rhs][s.field.spots[i].pos], rhs);
            });
        }
        BeamNode root;
        root.state = init;
        root.orderingScore =
            evaluate(root.state, s, ms, prev, spotDist, weights).total;
        fullEvals.fetch_add(1, std::memory_order_relaxed);
        root.orderKey = UINT64_C(1469598103934665603);

        auto considerPlan = [&](DailyPlan plan, bool requireScoreGain = false) {
            const SimulationResult sim = simulateDay(s, ms, plan, prev);
            if (!sim.valid) {
                return;
            }
            if (requireScoreGain && hasBest &&
                officialScore(sim.state) <= officialScore(res.finalState)) return;
            const EvaluationBreakdown eval = evaluate(
                sim.state, s, ms, prev, spotDist, weights);
            fullEvals.fetch_add(1, std::memory_order_relaxed);
            if (requireScoreGain) {
                if (!routeBest.success ||
                    officialScore(sim.state) > officialScore(routeBest.finalState) ||
                    (officialScore(sim.state) == officialScore(routeBest.finalState) &&
                     (eval.total > routeBest.evaluation.total ||
                      (eval.total == routeBest.evaluation.total &&
                       preferPlan(plan, routeBest.plan))))) {
                    routeBest.success = true;
                    routeBest.plan = std::move(plan);
                    routeBest.finalState = sim.state;
                    routeBest.evaluation = eval;
                }
                return;
            }
            if (hasBest &&
                (eval.total < res.evaluation.total ||
                 (eval.total == res.evaluation.total &&
                  !preferPlan(plan, res.plan)))) {
                return;
            }
            hasBest = true;
            res.success = true;
            res.plan = std::move(plan);
            res.finalState = sim.state;
            res.evaluation = eval;
        };
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

            considerPlan(std::move(plan));
            if (!cfg.completeIncumbentRoutes ||
                s.fuelLimit >= s.daySteps.front() ||
                std::all_of(node.routes.begin(), node.routes.end(),
                    [](ActionRoutes::Id id) { return id == ActionRoutes::none; }))
                return;
            plan = actionArena.materialize(node.planTails, node.state.agentCount);
            if (!appendRemainingRoutes(node.state, node.routes, routes, s, ms,
                                       plan, [&]() { return expired(); })) return;
            // 開始状態からの合法性と、確定得点の改善を確認してから保存する。
            considerPlan(std::move(plan), true);
        };
        if (hasBest) {
            res.evaluation = evaluate(
                res.finalState, s, ms, prev, spotDist, weights);
            fullEvals.fetch_add(1, std::memory_order_relaxed);
        }
        consider(root);
        if (expired(true)) {
            return finish(
                hasBest ? SearchStopReason::TimeLimit
                        : SearchStopReason::NoValidPlan);
        }

        const std::size_t dayStepCount = static_cast<std::size_t>(
            std::max(0, init.dayStepLimit));
        std::vector<std::vector<BeamNode>> eventBuckets(dayStepCount + 1);
        eventBuckets[0].push_back(std::move(root));
        std::vector<BeamNode> beam;
        std::vector<BeamNode> ready;
        std::vector<BeamNode> staged;
        std::vector<BeamNode> advanced;
        std::vector<std::tuple<int, ActionRoutes::Id, CellPos>> options;
        std::vector<ActionCandidate> candidates;
        std::size_t lastIncumbentStep = 0;
        std::size_t expansionRound = 0;
        ready.reserve(cfg.jointBeamWidth);
        staged.reserve(cfg.jointBeamWidth * 2);
        advanced.reserve(cfg.jointBeamWidth);

        for (std::size_t eventStep = 0;
             eventStep < dayStepCount;
             ++eventStep) {
            beam = std::move(eventBuckets[eventStep]);
            if (beam.empty()) {
                continue;
            }
            deduplicateAndLimit(
                beam,
                adaptiveLimit(cfg.beamWidth, cfg.minimumBeamWidth),
                s.field, cfg,
                uniqueIndices,
                dedupScratch);
            if (beam.empty()) {
                continue;
            }
            if (eventStep >=
                lastIncumbentStep + cfg.incumbentInterval) {
                consider(beam.front());
                lastIncumbentStep = eventStep;
                if (expired(true)) {
                    return finish(
                        hasBest ? SearchStopReason::TimeLimit
                                : SearchStopReason::NoValidPlan);
                }
            }
            ready.clear();
            ready.swap(beam);

            // 待機中でない各エージェントへ、合法な6方向移動または
            // 1ステップ待機を割り当てる。
            const AgentIndex firstAgent = static_cast<AgentIndex>(
                expansionRound++ % init.agentCount);
            for (AgentIndex order = 0;
                 order < init.agentCount;
                 ++order) {
                const AgentIndex a =
                    (firstAgent + order) % init.agentCount;
                staged.clear();
                staged.reserve(std::min(
                    ready.size() * 7,
                    cfg.jointBeamWidth * 2));
                for (const BeamNode& node : ready) {
                    if (node.state.pendingActions[a].active) {
                        staged.push_back(node);
                        continue;
                    }

                    if (node.routes[a] != ActionRoutes::none) {
                        const auto next = routes[node.routes[a]];
                        BeamNode child = node;
                        if (tryScheduleAction(child.state, s, ms, a, next.action).error ==
                            SimulationError::None) {
                            child.planTails[a] = actionArena.append(
                                node.planTails[a], next.action);
                            child.routes[a] = next.next;
                            if (next.next == ActionRoutes::none) child.aims[a] = kInvalidCell;
                            child.serial = nextSerial++;
                            ++res.macroContinuations;
                            staged.push_back(std::move(child));
                            // 継続候補は必ず残す。一定間隔では予約済み経路を
                            // 中断する候補も作り、在庫・合流の変化に追従する。
                            const CellPos aim = node.aims[a];
                            const bool depletedTarget =
                                node.state.agents[a].isPatrol() &&
                                s.field.contains(aim) &&
                                s.field.at(aim).hasSpot() &&
                                node.state.remainingStocks[
                                    s.field.at(aim).spotIndex] == 0;
                            const bool periodicReplan =
                                cfg.macroReplanInterval != 0 &&
                                eventStep % cfg.macroReplanInterval == 0;
                            bool staleSupplyRoute = false;
                            if (!node.state.agents[a].isPatrol()) {
                                const auto current = coordinatedAssignments(
                                    node.state, s, ms, spotDist);
                                const auto replacement = std::find_if(
                                    current.meets.begin(), current.meets.end(),
                                    [&](const RendezvousPlan& meet) {
                                        return meet.valid && meet.supply == a;
                                    });
                                staleSupplyRoute =
                                    replacement != current.meets.end() &&
                                    replacement->pos != aim;
                            }
                            if (!depletedTarget && !periodicReplan &&
                                !staleSupplyRoute)
                                continue;
                        }
                        // 補給予定が実現しなかった等の場合は、その場から再計画。
                    }

                    const auto assignment = coordinatedAssignments(node.state, s, ms, spotDist);
                    const auto& targets = assignment.targets;
                    const auto& meets = assignment.meets;

                    constexpr std::array<std::int32_t, 7> actions = {
                        0, 1, 2, 3, 4, 5, -1,
                    };
                    const bool forceWait =
                        static_cast<std::size_t>(node.state.day + 1) >=
                            s.dayCount() &&
                        shouldWaitAtSpot(node.state, a, s, targets);
                    options.clear();
                    for (const int action : actions)
                        options.emplace_back(action, ActionRoutes::none, kInvalidCell);
                    if (cfg.useMacroActions) {
                        if (node.state.remainingSteps() > 1)
                            options.emplace_back(-node.state.remainingSteps(), ActionRoutes::none, kInvalidCell);
                        if (!forceWait) {
                            for (const auto& plan : macroPlans(
                                     node.state, a, s, ms, spotDist, targets, meets, cfg)) {
                                const auto first = routes[routes.intern(plan)];
                                CellPos goal = node.state.agents[a].pos;
                                for (const auto action : plan)
                                    if (isMoveAction(action)) goal = s.field.neighbor(
                                        goal, static_cast<Direction>(action));
                                options.emplace_back(first.action, first.next, goal);
                            }
                        }
                    }
                    candidates.resize(options.size());
                    // 行動予約で変わるのは対象車のPendingActionだけ。
                    // 評価用状態を親ごとに一度コピーし、小さい予約情報を選別する。
                    GameState actionState = node.state;
                    std::size_t candidateCount = 0;
                    for (const auto& option : options) {
                        const int action = std::get<0>(option);
                        const auto continuation = std::get<1>(option);
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
                        actionState.pendingActions[a] = node.state.pendingActions[a];
                        const auto err = tryScheduleAction(
                            actionState,
                            s,
                            ms,
                            a,
                            action,
                            0);
                        if (err.error != SimulationError::None) {
                            continue;
                        }
                        candidate.pending = actionState.pendingActions[a];
                        candidate.action = action;
                        candidate.route = continuation;
                        candidate.aim = std::get<2>(option);
                        candidate.orderKey = appendOrderKey(
                            node.orderKey, a, action);
                        if (continuation != ActionRoutes::none) {
                            candidate.orderKey ^= continuation;
                            ++res.macroCandidates;
                        }
                        candidate.orderingScore =
                            node.orderingScore +
                            scheduledActionBonus(
                                actionState,
                                a,
                                s,
                                spotDist,
                                weights, targets, meets) +
                            (continuation != ActionRoutes::none ? 0.1 : 0.0);
                        ++candidateCount;
                        ++res.expandedNodes;
                    }

                    double bestActionScore =
                        -std::numeric_limits<double>::infinity();
                    for (std::size_t i = 0; i < candidateCount; ++i) {
                        bestActionScore = std::max(
                            bestActionScore,
                            candidates[i].orderingScore);
                    }
                    if (std::isfinite(bestActionScore)) {
                        for (std::size_t i = 0; i < candidateCount; ++i) {
                            // 親ごとの絶対的な誘導値を除き、最善行動からの
                            // 損失だけを加える。異なる親同士の比較を歪めない。
                            candidates[i].orderingScore =
                                node.orderingScore +
                                candidates[i].orderingScore -
                                bestActionScore;
                        }
                    }
                    std::size_t actionLimit = cfg.maxActionsPerAgent;
                    if (cfg.adaptiveActionLimit && actionLimit > 3 &&
                        (s.field.cellCount() >= 24U * 24U ||
                         init.agentCount >= 6)) {
                        actionLimit = 3;
                    }
                    const std::size_t keep =
                        actionLimit == 0
                            ? candidateCount
                            : std::min(
                                  candidateCount,
                                  actionLimit);
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
                        child.state.pendingActions[a] = candidate.pending;
                        child.routes[a] = candidate.route;
                        child.aims[a] = candidate.aim;
                        child.planTails[a] = actionArena.append(
                            node.planTails[a], candidate.action);
                        child.orderingScore = candidate.orderingScore;
                        child.orderKey = candidate.orderKey;
                        child.serial = nextSerial++;
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
                    std::int32_t delta = node.state.remainingSteps();
                    for (AgentIndex a = 0;
                         a < node.state.agentCount;
                         ++a) {
                        const PendingAction& pending =
                            node.state.pendingActions[a];
                        if (!pending.active || pending.remainingSteps <= 0) {
                            delta = 0;
                            break;
                        }
                        delta = std::min(delta, pending.remainingSteps);
                    }
                    if (delta <= 0) {
                        continue;
                    }

                    bool valid = true;
                    for (std::int32_t elapsed = 0;
                         elapsed < delta;
                         ++elapsed) {
                        if (expired()) {
                            evaluationTimedOut.store(
                                true, std::memory_order_relaxed);
                            return;
                        }
                        const auto err = advanceSimulationStep(node.state, s);
                        if (err.error != SimulationError::None) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) {
                        continue;
                    }
                    if (delta > 1) {
                        reusedEvals.fetch_add(
                            static_cast<std::size_t>(delta - 1),
                            std::memory_order_relaxed);
                    }
                    node.orderingScore = evaluate(
                        node.state, s, ms, prev, spotDist, weights).total;
                    fullEvals.fetch_add(
                        1, std::memory_order_relaxed);
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
            std::sort(advanced.begin(), advanced.end(), betterNode);
            std::vector<bool> touched(dayStepCount + 1, false);
            for (BeamNode& node : advanced) {
                const std::size_t nextEvent = static_cast<std::size_t>(
                    node.state.step);
                if (nextEvent <= eventStep || nextEvent > dayStepCount) {
                    continue;
                }
                eventBuckets[nextEvent].push_back(std::move(node));
                touched[nextEvent] = true;
            }
            // 長い待機マクロによる日末bucketへの蓄積を定期的に制限する。
            const auto bucketLimit = adaptiveLimit(cfg.beamWidth, cfg.minimumBeamWidth);
            for (std::size_t step = eventStep + 1; step <= dayStepCount; ++step) {
                if (touched[step] && eventBuckets[step].size() / 4 > bucketLimit)
                    deduplicateAndLimit(eventBuckets[step], bucketLimit,
                        s.field, cfg, uniqueIndices, dedupScratch);
            }
            if (expired(true)) {
                return finish(
                    hasBest ? SearchStopReason::TimeLimit
                            : SearchStopReason::NoValidPlan);
            }
        }

        beam = std::move(eventBuckets[dayStepCount]);
        if (beam.empty()) {
            return finish(
                hasBest ? SearchStopReason::Completed
                        : SearchStopReason::NoValidPlan);
        }

        deduplicateAndLimit(
            beam,
            adaptiveLimit(cfg.beamWidth, cfg.minimumBeamWidth),
            s.field, cfg,
            uniqueIndices,
            dedupScratch);

        EvaluationBreakdown finalEvaluation;
        const BeamNode* finalBest = nullptr;
        for (BeamNode& node : beam) {
            if (!node.state.finishDay()) {
                node.orderingScore =
                    -std::numeric_limits<double>::infinity();
                continue;
            }
            const auto evaluation = evaluate(node.state, s, ms, prev, spotDist, weights);
            node.orderingScore = evaluation.total;
            if (finalBest == nullptr || betterNode(node, *finalBest)) {
                finalBest = &node;
                finalEvaluation = evaluation;
            }
            fullEvals.fetch_add(1, std::memory_order_relaxed);
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
            if (!hasBest || best.orderingScore > res.evaluation.total ||
                (best.orderingScore == res.evaluation.total &&
                 preferPlan(finalPlan, res.plan))) {
                res.success = true;
                res.plan = finalPlan;
                res.finalState = best.state;
                res.evaluation = finalEvaluation;
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

}  // namespace benzen
