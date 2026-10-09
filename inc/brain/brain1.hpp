#pragma once

#include "brain0.hpp"
#include "../action_routes.hpp"
#include "../brand_search.hpp"
#include "../brand_repair.hpp"
#include "../udon_alns.hpp"
#include "../evaluation.hpp"
#include "../game_state_hash.hpp"
#include "../min_cost_flow.hpp"
#include "../plan_validator.hpp"
#include "../search_state.hpp"
#include "../simulator.hpp"
#include "../spot_route_lns.hpp"
#include "../route_column_search.hpp"
#include "../day_beam.hpp"
#include "../overnight_refinement.hpp"

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

using UdonRefiner = std::function<UdonAlnsResult(
    const MatchSetting&, const MatchState&, ScoreState, const DailyPlan&,
    const UdonAlnsConfig&, std::chrono::steady_clock::time_point)>;

struct BeamSearchConfig {
    // Experimental quantity portfolio. The multi-day beam is controlled
    // independently so the official cumulative daily-brand objective is
    // considered without enabling every quantity experiment.
    bool quantityUpgrades = false;
    // Fuel-aware end placement, anticipatory supply assignment and a reserved
    // quantity-refinement slice. Independent of the experimental day beam.
    bool coordinatedRefinement = true;
    // 系列上限未達のとき、枝刈り付き全行動探索で補完する。
    bool refineBrands = true;
    std::size_t brandSearchNodeLimit = 50000;
    // trueなら系列探索のノード上限を解除する。締切は引き続き守る。
    bool exactBrands = false;
    bool useBrandRepair = true;
    BrandRepairConfig brandRepair{};
    // Reserve a larger post-beam slice and widen repair/exact search while the
    // daily-brand upper bound is still open. Tight-fuel maps retain the proven
    // larger beam share because refuel coordination dominates there.
    bool adaptiveSmallBoardBrandRescue = true;
    std::size_t smallBoardBeamBudgetPercent = 75;
    std::size_t smallBoardBrandSearchNodeLimit = 200000;
    bool useUdonAlns = true;
    // 一回戦盤面では3巡回車の経路・在庫をまとめて再配分する。
    bool smallBoardJointRefinement = true;
    // 1969型の16×16・8日・14系列で、既存解を保持しながら
    // 玉数優先候補と系列優先候補を独立に比較する。
    bool smallBoardQuantityFirstRoutes = true;
    // 通常探索を終えた最終日だけ、残時間で幅広い経路組合せを比較。
    bool smallBoardFinalColumns = true;
    UdonAlnsConfig udonAlns{};
    // brain1 uses the established hill-climbing ALNS by default. A later
    // solver generation can inject another acceptance policy without copying
    // the beam/search pipeline.
    UdonRefiner udonRefiner{};
    // 本選3区分と実効探索時間からビーム／改善時間、ALNS上限を調整する。
    // falseなら従来の固定配分とUdonAlnsConfigをそのまま使う。
    bool adaptiveLongSearch = true;
    bool useSpotRouteLns = true;
    SpotRouteLnsConfig spotRouteLns{};
    RouteColumnConfig routeColumns{};
    bool compareNextDayPlans = false;
    std::int64_t nextDayProbeMs = 500;
    // Reserve time for quantity search and a bounded, scenario-based day beam.
    std::size_t quantityBudgetPercent = 25;
    bool stopBeamAtBrandUpperBound = true;
    bool useDayBeam = true;
    std::size_t dayBeamWidth = 4;
    std::size_t dayBeamDepth = 2;
    std::int32_t dayBeamMaxUdonLoss = 2;
    std::size_t beamWidth = 128;
    // Indicates size-calibrated widths; all modes share refinement reserves.
    bool autoSizedBeam = false;

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
    // 固定幅を早く完走した場合、締切まで幅と候補数を広げて再探索する。
    bool iterativeDeepening = true;
    // 追走ビームでは、完成した幅を広げても探索結果が連続して変わらなければ
    // 時刻上限を待たず、その安定幅の結果を採用する。
    bool stopOnStableBeam = false;
    std::size_t stableBeamPasses = 2;

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
    // 同一の公式得点を持つ中間日終端で、翌日距離と残燃料を比較する尺度。
    // 小さいほど残燃料を強く評価する。
    std::int32_t futureFuelDivisor = 20;
    std::int32_t tightFuelFutureDivisor = 10;
    bool useMacroActions = true;
    std::size_t macroTargetLimit = 3;
    // マクロ継続を残しつつ、定期的に別経路も探索する。0なら再検討しない。
    std::size_t macroReplanInterval = 0;
    // 最終日に十分な時間がある場合、盤面サイズ・台数によらず
    // 既定経路と定期再計画経路を探索し、公式得点が高い方を採用する。
    bool comparePeriodicReplanPlans = true;
    std::int64_t routePortfolioMinTimeMs = 5000;
    // 共同割当の変化で旧巡回先の在庫が他車に予約済み、またはより重要な
    // 系列が割り当てられた場合、マクロ継続と再計画を併せて探索する。
    bool reassignStalePatrolRoutes = true;
    // 探索後、最終状態を変えない補給車の周回を待機へ置換する。
    bool simplifySupplyRoutes = true;
    // 24x24以上で合流割当がない補給車の自由移動を許す検証用フラグ。
    // 既定では低燃料の巡回車へ近づく移動だけを優先する。
    bool allowUnassignedSupplyMoves = false;
    // An unassigned supply must wake up periodically.  A wait covering the
    // whole remaining day cannot be cancelled when patrol targets change.
    std::size_t maxSupplyWaitSteps = 8;
    bool coordinatedAssignments = true;
    bool detourRendezvous = true;
    bool twoSpotLookahead = true;
    // 最良の2店目だけでなく、在庫競合時の代替候補も巡回先評価へ反映する。
    bool robustRouteLookahead = true;
    // 同じ補給地点へ向かう複数巡回車を1台の補給車へまとめて割り当てる。
    bool sharedRendezvous = true;
    // 翌日の配置では、全巡回車から遠いスポットを先に担当させる。
    bool prioritizeHardNextDayTargets = true;
    std::size_t assignmentCacheLimit = 4096;
    // 同一地形・交通条件の距離前計算を呼び出しスレッド内で再利用する。
    // 0なら無効。状態や締切・割当キャッシュは保存しない。
    std::size_t distanceCacheEntries = 8;
    std::size_t detourCandidateLimit = 6;
    // 翌日配置とのトレードオフがない最終日は迂回合流を広く比較する。
    std::size_t finalDayDetourCandidateLimit = 8;
    bool exhaustFeasibleDetours = true;
    std::size_t lookaheadCandidateLimit = 6;
    bool profilePlanning = false;
    bool dominancePruning = true;
    // 系列・位置が同じでも、各車の取得済みスポットが異なる候補を残す。
    bool diversifyAcquiredSpots = true;
    // 短時間探索ではニッチ増加より既存候補の深掘りを優先する。
    std::int64_t quantityEnhancementsMinTimeMs = 500;
    // 上位解を残しつつ、この割合を異なる系列・位置の候補へ確保する。
    double diversityFraction = 0.25;
};

namespace detail {

inline bool isSharplyShorterNextDay(
    const MatchSetting& setting, std::int32_t day) noexcept {
    if (day < 0) return false;
    const auto current = static_cast<std::size_t>(day);
    const auto next = current + 1;
    return next < setting.daySteps.size() &&
        3LL * setting.daySteps[next] <=
            2LL * setting.daySteps[current];
}

inline bool isFuelRiskDay(const MatchSetting& setting,
                          const MatchState& matchState) {
    if (matchState.day < 0 ||
        static_cast<std::size_t>(matchState.day) >= setting.daySteps.size())
        return false;
    if (static_cast<std::int64_t>(setting.fuelLimit) * 2 <=
        setting.daySteps[static_cast<std::size_t>(matchState.day)])
        return true;
    const int lowFuel = std::max(2, setting.fuelLimit / 10);
    std::int64_t totalFuel = 0;
    std::int64_t patrols = 0;
    for (const Agent& agent : matchState.agents) {
        if (!agent.isPatrol()) continue;
        ++patrols;
        totalFuel += agent.fuel;
        if (agent.fuel <= lowFuel) return true;
        bool canMove = false;
        for (int raw = 0; raw < 6; ++raw) {
            const CellPos next = setting.field.neighbor(
                agent.pos, static_cast<Direction>(raw));
            if (!setting.field.contains(next) ||
                !setting.field.at(next).isPassable()) continue;
            if (moveFuelCost(setting, AgentKind::Patrol, agent.pos) <=
                agent.fuel) {
                canMove = true;
                break;
            }
        }
        if (!canMove) return true;
    }
    return patrols > 0 && totalFuel * 3 <
        patrols * static_cast<std::int64_t>(setting.fuelLimit);
}

struct AdaptiveLongSearchProfile {
    std::size_t quantityBudgetPercent = 25;
    // 改善枠のうちALNSへ残す割合。残りはスポット・経路探索が使う。
    std::size_t alnsPercentOfRefinement = 25;
    UdonAlnsConfig udonAlns{};
};

inline AdaptiveLongSearchProfile adaptiveLongSearchProfile(
    const MatchSetting& setting, const BeamSearchConfig& config,
    std::int64_t effectiveBudgetMs, bool finalDay, bool severeFuel) {
    AdaptiveLongSearchProfile profile;
    profile.quantityBudgetPercent = std::clamp<std::size_t>(
        config.quantityBudgetPercent, 1, 90);
    profile.udonAlns = config.udonAlns;
    if (config.smallBoardJointRefinement &&
        setting.field.width == 16 && setting.field.height == 16) {
        profile.udonAlns.maxPatrols = std::max<std::size_t>(
            profile.udonAlns.maxPatrols, 3);
    }
    if (!config.adaptiveLongSearch || effectiveBudgetMs <= 0 ||
        effectiveBudgetMs == std::numeric_limits<std::int64_t>::max() ||
        config.quantityUpgrades) {
        return profile;
    }

    // 先行端末では早い合法解を優先し、追走端末ほど改善枠を厚くする。
    // 厳燃料日は補給計画を浅くしないよう主ビームを優先する。
    if (severeFuel) {
        profile.quantityBudgetPercent = 15;
        profile.alnsPercentOfRefinement = 20;
    } else if (effectiveBudgetMs < 15000) {
        profile.quantityBudgetPercent = 18;
        profile.alnsPercentOfRefinement = 20;
    } else if (effectiveBudgetMs < 30000) {
        profile.quantityBudgetPercent = 25;
        profile.alnsPercentOfRefinement = 30;
    } else if (effectiveBudgetMs < 45000) {
        profile.quantityBudgetPercent = finalDay ? 30 : 27;
        profile.alnsPercentOfRefinement = finalDay ? 40 : 30;
    } else {
        profile.quantityBudgetPercent = finalDay ? 35 : 30;
        profile.alnsPercentOfRefinement = finalDay ? 45 : 35;
    }

    if (effectiveBudgetMs < 15000) return profile;
    const auto size = std::max(setting.field.width, setting.field.height);
    const std::size_t tier = size >= 32 ? 2 : (size >= 24 ? 1 : 0);
    const std::array<std::size_t, 3> mediumAttempts{{16, 18, 20}};
    const std::array<std::size_t, 3> longAttempts{{18, 21, 24}};
    const std::array<std::size_t, 3> fullAttempts{{20, 24, 28}};
    const std::array<std::size_t, 3> mediumNodes{{1200, 1100, 1000}};
    const std::array<std::size_t, 3> longNodes{{1500, 1350, 1200}};
    const std::array<std::size_t, 3> fullNodes{{1800, 1600, 1400}};
    if (effectiveBudgetMs < 30000) {
        profile.udonAlns.maxAttempts = std::max(
            profile.udonAlns.maxAttempts, mediumAttempts[tier]);
        profile.udonAlns.nodesPerAttempt = std::max(
            profile.udonAlns.nodesPerAttempt, mediumNodes[tier]);
        profile.udonAlns.improvementBonusAttempts = std::max<std::size_t>(
            profile.udonAlns.improvementBonusAttempts, 5);
    } else if (effectiveBudgetMs < 45000) {
        profile.udonAlns.maxAttempts = std::max(
            profile.udonAlns.maxAttempts, longAttempts[tier]);
        profile.udonAlns.nodesPerAttempt = std::max(
            profile.udonAlns.nodesPerAttempt, longNodes[tier]);
        profile.udonAlns.improvementBonusAttempts = std::max<std::size_t>(
            profile.udonAlns.improvementBonusAttempts, 6);
    } else {
        profile.udonAlns.maxAttempts = std::max(
            profile.udonAlns.maxAttempts, fullAttempts[tier]);
        profile.udonAlns.nodesPerAttempt = std::max(
            profile.udonAlns.nodesPerAttempt, fullNodes[tier]);
        profile.udonAlns.improvementBonusAttempts = std::max<std::size_t>(
            profile.udonAlns.improvementBonusAttempts, 8);
    }
    return profile;
}

} // namespace detail

enum class SearchStopReason : std::uint8_t {
    Completed,
    TimeLimit,
    NoValidPlan,
};

struct BeamSearchResult {
    // 指定種別・当日の交通に対する (試合系列数, 当日系列数) の証明。
    bool brandOptimal = false;
    std::uint32_t matchBrandUpperBound = 0;
    std::uint32_t dailyBrandUpperBound = 0;
    std::size_t brandSearchExpanded = 0;
    std::size_t brandRepairAttempts = 0;
    std::size_t brandRepairExpanded = 0;
    std::size_t brandRepairImprovements = 0;
    std::size_t udonAlnsAttempts = 0;
    std::size_t udonAlnsExpanded = 0;
    std::size_t udonAlnsImprovements = 0;
    std::size_t udonAlnsAcceptedMoves = 0;
    std::size_t udonAlnsAcceptedWorseMoves = 0;
    std::size_t udonAlnsGreedyAttempts = 0;
    std::size_t udonJointDestroyAttempts = 0;
    std::size_t udonJointDestroyExpanded = 0;
    std::size_t udonJointDestroyImprovements = 0;
    std::array<std::size_t, 7> udonAlnsNeighborhoodAttempts{};
    std::array<std::size_t, 7> udonAlnsNeighborhoodAccepted{};
    bool stableBeamStopped = false;
    bool udonSaStagnationStopped = false;
    std::size_t selectedBeamWidth = 0;
    std::size_t quantityBudgetPercentUsed = 0;
    std::size_t beamBudgetPercentUsed = 0;
    std::size_t udonAlnsAttemptLimit = 0;
    std::size_t udonAlnsNodesPerAttempt = 0;
    std::size_t spotRouteLnsAttempts = 0;
    std::size_t spotRouteLnsImprovements = 0;
    std::size_t spotPortfolioMaxBrands = 0;
    std::size_t spotPortfolioMaxUdon = 0;
    std::size_t spotPortfolioMaxValidUdon = 0;
    std::size_t routeColumnsGenerated = 0;
    std::size_t routeColumnsMasterNodes = 0;
    std::size_t routeColumnsImprovements = 0;
    bool smallBoardBrandRescue = false;
    std::size_t dayBeamExpanded = 0;
    std::size_t overnightAttempts = 0;
    std::size_t overnightImprovements = 0;
    std::size_t dayBeamCandidates = 0;
    bool dayBeamComplete = false;
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
    std::size_t removedSupplyLoops = 0;
    std::array<SupplyUtilization, kMaxAgents> supplyUtilization{};
    std::int64_t assignmentTimeNs = 0;
    std::int64_t flowTimeNs = 0;
    std::int64_t rendezvousTimeNs = 0;
    SearchStopReason stopReason = SearchStopReason::NoValidPlan;
    std::int64_t elapsedMs = 0;

    // Add counters that describe work performed while searching.  Keeping this
    // in one place prevents portfolio and iterative-deepening wrappers from
    // silently dropping a newly added metric.
    void addSearchWork(const BeamSearchResult& other) noexcept {
        expandedNodes += other.expandedNodes;
        fullEvaluations += other.fullEvaluations;
        reusedEvaluations += other.reusedEvaluations;
        macroCandidates += other.macroCandidates;
        macroContinuations += other.macroContinuations;
        assignmentCacheHits += other.assignmentCacheHits;
        assignmentComputations += other.assignmentComputations;
        repairedAssignments += other.repairedAssignments;
        detourPlans += other.detourPlans;
        removedSupplyLoops += other.removedSupplyLoops;
        assignmentTimeNs += other.assignmentTimeNs;
        flowTimeNs += other.flowTimeNs;
        rendezvousTimeNs += other.rendezvousTimeNs;
        reusedDistanceTables =
            reusedDistanceTables || other.reusedDistanceTables;
    }

    void copySearchWork(const BeamSearchResult& other) noexcept {
        expandedNodes = other.expandedNodes;
        fullEvaluations = other.fullEvaluations;
        reusedEvaluations = other.reusedEvaluations;
        macroCandidates = other.macroCandidates;
        macroContinuations = other.macroContinuations;
        assignmentCacheHits = other.assignmentCacheHits;
        assignmentComputations = other.assignmentComputations;
        repairedAssignments = other.repairedAssignments;
        detourPlans = other.detourPlans;
        removedSupplyLoops = other.removedSupplyLoops;
        assignmentTimeNs = other.assignmentTimeNs;
        flowTimeNs = other.flowTimeNs;
        rendezvousTimeNs = other.rendezvousTimeNs;
        reusedDistanceTables = other.reusedDistanceTables;
    }
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
        bool lightTraffic = true;
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
            workers = std::min(
                count_, std::max<std::size_t>(workers, 1));
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

public:
    static std::int32_t supplyDecisionWait(
        const GameState& state, AgentIndex supply,
        std::size_t maximumWaitSteps) noexcept {
        if (supply >= state.agentCount ||
            !state.agents[supply].isSupply() ||
            state.remainingSteps() <= 0) return 0;
        std::int32_t wait = state.remainingSteps();
        if (maximumWaitSteps > 0) {
            wait = std::min<std::int32_t>(wait,
                static_cast<std::int32_t>(std::min<std::size_t>(
                    maximumWaitSteps,
                    static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max()))));
        }
        // Reconsider as soon as any patrol completes its current action.  Its
        // next target and fuel demand may create a new rendezvous assignment.
        for (AgentIndex p = 0; p < state.agentCount; ++p) {
            if (!state.agents[p].isPatrol()) continue;
            const auto& pending = state.pendingActions[p];
            if (pending.active) wait = std::min(wait, pending.remainingSteps);
        }
        return std::max<std::int32_t>(1, wait);
    }

    // 補給車が同じ地点へ戻る区間を待機へ置換する。全体を再シミュレーションし、
    // 巡回車の燃料・取得・終端位置を含む最終状態が一致する場合だけ採用する。
    static std::size_t simplifySupplyPlan(
        const MatchSetting& setting, const MatchState& matchState,
        ScoreState previousScore, const GameState& expected,
        DailyPlan& plan) {
        struct Loop {
            std::size_t begin = 0;
            std::size_t end = 0;
            std::int32_t duration = 0;
        };
        std::size_t removed = 0;
        std::size_t checks = 0;
        constexpr std::size_t maxChecks = 128;
        for (AgentIndex agent = 0;
             agent < plan.size() && agent < matchState.agents.size(); ++agent) {
            if (!matchState.agents[agent].isSupply()) continue;
            bool changed = true;
            while (changed) {
                changed = false;
                const AgentPlan& actions = plan[agent];
                std::vector<CellPos> positions(actions.size() + 1);
                std::vector<std::int32_t> elapsed(actions.size() + 1);
                std::vector<std::size_t> moves(actions.size() + 1);
                positions[0] = matchState.agents[agent].pos;
                for (std::size_t i = 0; i < actions.size(); ++i) {
                    positions[i + 1] = positions[i];
                    elapsed[i + 1] = elapsed[i];
                    moves[i + 1] = moves[i];
                    if (isMoveAction(actions[i])) {
                        elapsed[i + 1] += moveStepCost(
                            setting, matchState, positions[i]);
                        positions[i + 1] = setting.field.neighbor(
                            positions[i], static_cast<Direction>(actions[i]));
                        ++moves[i + 1];
                    } else {
                        elapsed[i + 1] -= actions[i];
                    }
                }
                std::vector<Loop> loops;
                for (std::size_t begin = 0; begin < actions.size(); ++begin) {
                    for (std::size_t end = begin + 1;
                         end <= actions.size(); ++end) {
                        if (positions[begin] != positions[end] ||
                            moves[end] - moves[begin] < 2) {
                            continue;
                        }
                        loops.push_back({
                            begin, end, elapsed[end] - elapsed[begin]});
                    }
                }
                std::sort(loops.begin(), loops.end(),
                    [](const Loop& lhs, const Loop& rhs) {
                        const auto left = lhs.end - lhs.begin;
                        const auto right = rhs.end - rhs.begin;
                        return left != right ? left > right
                                             : lhs.begin < rhs.begin;
                    });
                for (const Loop& loop : loops) {
                    if (checks++ >= maxChecks) return removed;
                    AgentPlan candidate;
                    candidate.reserve(
                        actions.size() - (loop.end - loop.begin) + 1);
                    candidate.insert(candidate.end(), actions.begin(),
                                     actions.begin() + loop.begin);
                    appendPlanAction(candidate, -loop.duration);
                    candidate.insert(candidate.end(), actions.begin() + loop.end,
                                     actions.end());
                    DailyPlan trialPlan = plan;
                    trialPlan[agent] = std::move(candidate);
                    const SimulationResult trial = simulateDay(
                        setting, matchState, trialPlan, previousScore);
                    if (!trial.valid || trial.state != expected) continue;
                    plan = std::move(trialPlan);
                    ++removed;
                    changed = true;
                    break;
                }
            }
        }
        return removed;
    }

private:

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
        bool diversifyAcquiredSpots,
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
                if (diversifyAcquiredSpots &&
                    node.state.agents[a].isPatrol()) {
                    detail::hashValue(hash, node.state.acquiredSpots[a]);
                }
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
        std::array<std::int32_t, kMaxSpots> futureDifficulty{};
        futureDifficulty.fill(0);
        for (auto& row : edges) row.fill(-1);
        const std::int64_t timeBound = std::max<std::int64_t>(
            state.dayStepLimit, 4LL * setting.field.cellCount());
        // 全車分の時間・先読み誘導値の差より1玉の報酬を大きくする。
        const std::int64_t pickup = timeBound *
            (currentDay && dist.planning.twoSpotLookahead ? 2 * n + 1 : n + 1) + 1;
        const std::int64_t daily = pickup * (n + 1);
        const std::int64_t fresh = daily * (n + 1);
        const auto currentDayIndex = state.day >= 0
            ? static_cast<std::size_t>(state.day) : setting.daySteps.size();
        const auto nextDayIndex = state.day >= 0
            ? static_cast<std::size_t>(state.day + 1) : setting.daySteps.size();
        const bool hasNextDay = currentDayIndex < setting.daySteps.size() &&
            nextDayIndex < setting.daySteps.size();
        const bool sharplyShorterNextDay =
            detail::isSharplyShorterNextDay(setting, state.day);
        const bool finalTightFuelDay = hasNextDay &&
            setting.field.cellCount() >= 32U * 32U && dist.lightTraffic &&
            nextDayIndex + 1 >= setting.dayCount() &&
            5LL * setting.fuelLimit <=
                6LL * setting.daySteps[nextDayIndex];
        const bool prioritizeHardFuture =
            !currentDay && dist.planning.prioritizeHardNextDayTargets &&
            (sharplyShorterNextDay || finalTightFuelDay);
        if (prioritizeHardFuture) {
            std::array<std::int32_t, kMaxBrands> spotsPerBrand{};
            for (const Spot& spot : setting.field.spots) {
                ++spotsPerBrand[static_cast<std::size_t>(spot.brandIndex)];
            }
            for (int i = 0; i < spots; ++i) {
                const auto brand = static_cast<std::size_t>(
                    setting.field.spots[static_cast<std::size_t>(i)].brandIndex);
                if (spotsPerBrand[brand] != 1) continue;
                std::int32_t nearest = kImpassableCost;
                for (int a = 0; a < n; ++a) {
                    if (!state.agents[a].isPatrol()) continue;
                    nearest = std::min(
                        nearest,
                        dist.steps[i][projectedPosition(state, a)]);
                }
                const auto nextDay = static_cast<std::size_t>(state.day + 1);
                if (nearest < kImpassableCost &&
                    nextDay < setting.daySteps.size() &&
                    4LL * nearest >= setting.daySteps[nextDay]) {
                    futureDifficulty[i] = nearest;
                }
            }
        }
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
                std::int64_t alternative = 0;
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
                        const std::int64_t candidate = std::min(timeBound / 2,
                            static_cast<std::int64_t>(value) *
                            (left - second->steps + 1));
                        if (candidate > continuation) {
                            alternative = continuation;
                            continuation = candidate;
                        } else {
                            alternative = std::max(alternative, candidate);
                        }
                    }
                    // 最良候補が別車に取られても巡回を継続できる経路を優先する。
                    // 主候補より弱く加点し、直近1玉の価値は決して覆さない。
                    if (dist.planning.robustRouteLookahead)
                        continuation += alternative / 16;
                }
                times[a][i] = travel;
                const std::int64_t preparation =
                    prioritizeHardFuture
                        ? 4LL * futureDifficulty[i] : 0;
                edges[a][i] = flow.addEdge(
                    a + 1, spotBase + i, 1,
                    travel - pickup - continuation - preparation);
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
            const bool exhaustFeasible =
                dist.planning.exhaustFeasibleDetours &&
                !setting.daySteps.empty() &&
                setting.fuelLimit <= setting.daySteps.front();
            if (!exhaustFeasible) {
                const auto retained = std::min(
                    candidates.size(), dist.planning.detourCandidateLimit);
                std::partial_sort(candidates.begin(), candidates.begin() +
                    static_cast<std::ptrdiff_t>(retained), candidates.end());
                candidates.resize(retained);
            }
            // 燃料制約が厳しい場合は下界の小さい順に必要な分だけ取り出し、
            // 合流点へ届かない候補を実経路の確認数へ含めない。
            std::make_heap(
                candidates.begin(), candidates.end(), std::greater<Candidate>{});
            std::size_t checked = 0;
            while (!candidates.empty() &&
                   checked < dist.planning.detourCandidateLimit) {
                if (dist.stop && dist.stop()) break;
                std::pop_heap(
                    candidates.begin(), candidates.end(), std::greater<Candidate>{});
                const Candidate entry = candidates.back();
                candidates.pop_back();
                if (!exhaustFeasible) ++checked;
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
                if (exhaustFeasible) ++checked;
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
                const bool urgent = meet.required ||
                    (dist.planning.coordinatedRefinement && standbyFuelShortage > 0);
                const std::int64_t reward = benefit +
                    (urgent ? benefit * (n + 1) : 0) +
                    (urgent && targets[p].valid
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
        std::array<bool, kMaxAgents> usedSupply{};
        for (int p = 0; p < n; ++p) {
            for (int q = 0; q < n; ++q) {
                if (edges[p][q] >= 0 && flow.capacity(p + 1, edges[p][q]) == 0) {
                    result[p] = pairs[p][q];
                    usedSupply[q] = true;
                }
            }
        }
        if (dist.planning.sharedRendezvous) {
            // 公式ルールでは同じセルの補給車1台が複数巡回車を同時に
            // 満タンにできる。一対一フローで主担当を決めた後、同じ補給経路を
            // 共有できる未割当車を追加し、異なる地点への二重割当は避ける。
            for (int p = 0; p < n; ++p) {
                if (!state.agents[p].isPatrol() || result[p].valid) continue;
                for (int q = 0; q < n; ++q) {
                    if (!usedSupply[q] || !pairs[p][q].valid) continue;
                    const auto primary = std::find_if(
                        result.begin(), result.end(), [&](const RendezvousPlan& meet) {
                            return meet.valid &&
                                   meet.supply == static_cast<AgentIndex>(q);
                        });
                    if (primary == result.end() ||
                        primary->pos != pairs[p][q].pos ||
                        primary->supplyArrival != pairs[p][q].supplyArrival) {
                        continue;
                    }
                    result[p] = pairs[p][q];
                    break;
                }
            }
        }
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
        const double fuelPressure = std::min(
            1.0, std::max(0.0, (1.5 - fuelRatio) / 1.5));
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
            } else if (!spotDistances.planning.allowUnassignedSupplyMoves &&
                       setting.field.cellCount() >= 24U * 24U) {
                // 合流相手がいないときに全方向の移動を候補上位へ置くと、
                // 割当が生じるまで補給車が無目的に周回する。一方、完全に
                // 停止すると次の補給への先回りを失うため、目的地を持つ巡回車
                // のうち燃料が最も少ない1台へ近づく移動だけを優遇する。
                AgentIndex follow = state.agentCount;
                std::int32_t leastFuel = std::numeric_limits<std::int32_t>::max();
                std::size_t supplyCount = 0;
                for (AgentIndex patrolIndex = 0;
                     patrolIndex < state.agentCount; ++patrolIndex) {
                    supplyCount += state.agents[patrolIndex].isSupply();
                    if (!state.agents[patrolIndex].isPatrol() ||
                        !targets[patrolIndex].valid) {
                        continue;
                    }
                    const std::int32_t fuel = projectedFuel(
                        state, patrolIndex);
                    if (fuel < leastFuel) {
                        leastFuel = fuel;
                        follow = patrolIndex;
                    }
                }
                bonus = 0.0;
                if (pending.isWait() && pending.remainingSteps > 1 &&
                    supplyCount > 1 &&
                    state.step * 32 < state.dayStepLimit) {
                    // 複数補給車では一対一割当の競合により、日序盤だけ合流先が
                    // 一時的に消えることがある。残り時間すべてを待つと次の移動
                    // 完了時に再評価できないため、短い待機を残して長時間待機を
                    // 候補上位から外す。終盤は役目を終えた車を無理に動かさない。
                    bonus = -1.0 - 0.01 * pending.remainingSteps;
                } else if (!pending.isWait() && follow < state.agentCount) {
                    const CellPos patrolPos = projectedPosition(state, follow);
                    const std::int32_t before =
                        spotDistances.travel[agent.pos][patrolPos];
                    const std::int32_t after = spotDistances.travel[
                        projectedPosition(state, agentIndex)][patrolPos];
                    if (before < kImpassableCost && after < before) {
                        bonus = -weights.supplyDistance *
                                    static_cast<double>(before - after) -
                                0.01 * pending.remainingSteps;
                    } else {
                        bonus = -1.0 - 0.01 * pending.remainingSteps;
                    }
                }
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
        const bool finalDay = ms.day >= 0 &&
            static_cast<std::size_t>(ms.day + 1) >= s.dayCount();
        const bool tightFuelDay = detail::isFuelRiskDay(s, ms);
        const int futureFuelDivisor = tightFuelDay
            ? cfg.tightFuelFutureDivisor : cfg.futureFuelDivisor;
        const auto officialScore = [](const GameState& state) {
            return std::make_tuple(countBits(state.score.matchBrands),
                state.score.sumOfDailyBrandCounts, state.score.totalUdon);
        };
        std::vector<DijkstraTable> selectionCache(s.field.cellCount());
        std::vector<bool> selectionCached(s.field.cellCount(), false);
        const auto selectionScore = [&](const GameState& state) {
            const auto future = finalDay ? detail::FutureRoutePotential{} :
                detail::futureRoutePotential(s, ms, state,
                    selectionCache, selectionCached);
            // Once the combined plan keeps the best known brand counts,
            // prefer actual pickups. Future position is only a tie-breaker.
            return std::make_tuple(countBits(state.score.matchBrands),
                state.score.sumOfDailyBrandCounts,
                state.score.totalUdon,
                future.reachableBrands,
                -future.strandedPatrols, future.readyBrands, future.readyUdon,
                finalDay ? 0 : -future.distanceSum +
                    future.patrolFuel / futureFuelDivisor);
        };
        auto cache = std::make_shared<AssignmentCache>();
        auto timings = std::make_shared<PlanningTimings>();
        std::atomic<std::size_t> fullEvals{0};
        std::atomic<std::size_t> reusedEvals{0};
        auto finish = [&](SearchStopReason reason) {
            // 経路完了案は従来の途中解を上書きせず別に保持する。
            // 全日で系列数を最優先する。中間日の獲得数は翌日配置との
            // トレードオフがあるため、同じ系列数なら終端評価で比較する。
            if (routeBest.success && (!res.success ||
                selectionScore(routeBest.finalState) >
                    selectionScore(res.finalState) ||
                (selectionScore(routeBest.finalState) ==
                     selectionScore(res.finalState) &&
                 routeBest.evaluation.total > res.evaluation.total))) {
                res.success = true;
                res.plan = std::move(routeBest.plan);
                res.finalState = routeBest.finalState;
                res.evaluation = routeBest.evaluation;
            }
            if (res.success) {
                if (cfg.simplifySupplyRoutes) {
                    res.removedSupplyLoops += simplifySupplyPlan(
                        s, ms, prev, res.finalState, res.plan);
                }
                res.plan = compressWaitActions(res.plan);
                const auto replay = simulateDay(s, ms, res.plan, prev);
                if (replay.valid) {
                    res.supplyUtilization = replay.supplyUtilization;
                }
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
            cfg.routePortfolioMinTimeMs < 0 ||
            cfg.quantityEnhancementsMinTimeMs < 0 ||
            cfg.futureFuelDivisor <= 0 ||
            cfg.tightFuelFutureDivisor <= 0 ||
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
        const bool useQuantityEnhancements =
            !limited || budgetMs >= cfg.quantityEnhancementsMinTimeMs;
        const bool diversifyAcquiredSpots =
            cfg.diversifyAcquiredSpots && useQuantityEnhancements;
        const auto deadline = limited
                                  ? started + std::chrono::milliseconds(budgetMs)
                                  : Clock::time_point::max();
        const std::size_t hardwareThreads = std::min<std::size_t>(
            8,
            std::max(1U, std::thread::hardware_concurrency()));
        const std::size_t requestedThreads =
            cfg.threadCount == 0 ? hardwareThreads : cfg.threadCount;
        const std::size_t threadCount = std::min(
            hardwareThreads,
            std::max<std::size_t>(requestedThreads, 1));
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
        if (!useQuantityEnhancements) {
            spotDist.planning.robustRouteLookahead = false;
            spotDist.planning.sharedRendezvous = false;
        }
        if (finalDay) {
            spotDist.planning.detourCandidateLimit = std::max(
                spotDist.planning.detourCandidateLimit,
                cfg.finalDayDetourCandidateLimit);
        }
        {
            std::size_t roads = 0, affected = 0;
            for (std::size_t pos = 0;
                 pos < s.field.cellCount() && pos < ms.trafficByPos.size(); ++pos) {
                if (s.field.cells[pos].type != CellType::Road) continue;
                ++roads;
                if (ms.trafficByPos[pos] != TrafficStatus::Smooth) ++affected;
            }
            spotDist.lightTraffic = roads == 0 || affected * 10 <= roads;
        }
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
            if (hasBest) {
                const auto score = selectionScore(sim.state);
                const auto bestScore = selectionScore(res.finalState);
                if (score < bestScore ||
                    (score == bestScore &&
                     (eval.total < res.evaluation.total ||
                      (eval.total == res.evaluation.total &&
                       !preferPlan(plan, res.plan))))) {
                    return;
                }
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
                diversifyAcquiredSpots,
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
                            bool stalePatrolRoute = false;
                            bool staleSupplyRoute = false;
                            if (node.state.agents[a].isPatrol() &&
                                cfg.reassignStalePatrolRoutes &&
                                s.field.contains(aim) &&
                                s.field.at(aim).hasSpot()) {
                                const auto current = coordinatedAssignments(
                                    node.state, s, ms, spotDist);
                                const auto target = current.targets[a];
                                const std::size_t oldSpot = static_cast<std::size_t>(
                                    s.field.at(aim).spotIndex);
                                if (target.valid &&
                                    s.field.spots[target.spotIndex].pos != aim) {
                                    const auto oldBit = brandBit(
                                        s.field.spots[oldSpot].brandIndex);
                                    const int oldPriority =
                                        (node.state.score.matchBrands & oldBit) == 0 ? 0 :
                                        ((node.state.dayBrands & oldBit) == 0 ? 1 : 2);
                                    std::int32_t reservations = 0;
                                    for (AgentIndex p = 0;
                                         p < node.state.agentCount; ++p) {
                                        reservations += current.targets[p].valid &&
                                            current.targets[p].spotIndex == oldSpot;
                                    }
                                    stalePatrolRoute =
                                        target.priority < oldPriority ||
                                        reservations >=
                                            node.state.remainingStocks[oldSpot];
                                }
                            } else if (!node.state.agents[a].isPatrol()) {
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
                                !stalePatrolRoute &&
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
                        if (node.state.remainingSteps() > 1) {
                            const auto wait = node.state.agents[a].isSupply()
                                ? supplyDecisionWait(node.state, a,
                                      cfg.maxSupplyWaitSteps)
                                : node.state.remainingSteps();
                            options.emplace_back(
                                -wait, ActionRoutes::none, kInvalidCell);
                        }
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
                        s.field, cfg, diversifyAcquiredSpots,
                        uniqueIndices, dedupScratch);
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
            diversifyAcquiredSpots,
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
            if (finalBest == nullptr ||
                selectionScore(node.state) > selectionScore(finalBest->state) ||
                (selectionScore(node.state) == selectionScore(finalBest->state) &&
                 betterNode(node, *finalBest))) {
                finalBest = &node;
                finalEvaluation = evaluation;
            }
            fullEvals.fetch_add(1, std::memory_order_relaxed);
        }
        if (finalBest != nullptr) {
            if (finalBest != &beam.front()) {
                std::iter_swap(beam.begin(),
                    beam.begin() + (finalBest - beam.data()));
            }
            beam.resize(1);
        } else {
            sortAndLimit(beam, 1);
        }
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
            const auto score = selectionScore(best.state);
            const auto bestScore = selectionScore(res.finalState);
            if (!hasBest || score > bestScore ||
                (score == bestScore &&
                 (best.orderingScore > res.evaluation.total ||
                  (best.orderingScore == res.evaluation.total &&
                   preferPlan(finalPlan, res.plan))))) {
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

inline BeamSearchResult brain1Pass(
    const MatchSetting& setting,
    const MatchState& matchState,
    ScoreState previousScore = {},
    const BeamSearchConfig& config = {}) {
    const bool unlimited = config.timeLimitMs == 0 &&
        (!config.useStateDeadline || matchState.endsAt <= 0);
    const bool compareRoutes = config.comparePeriodicReplanPlans &&
        config.useMacroActions && config.macroReplanInterval == 0 &&
        matchState.day >= 0 &&
        static_cast<std::size_t>(matchState.day + 1) >= setting.dayCount() &&
        (unlimited || config.timeLimitMs >= config.routePortfolioMinTimeMs);
    if (!compareRoutes) {
        return Brain1::search(setting, matchState, previousScore, config);
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    BeamSearchConfig primaryConfig = config;
    if (!unlimited) {
        const std::int64_t effective = std::max<std::int64_t>(
            0, config.timeLimitMs - config.safetyMarginMs);
        primaryConfig.timeLimitMs = config.safetyMarginMs + effective / 2;
    }
    BeamSearchResult primary = Brain1::search(
        setting, matchState, previousScore, primaryConfig);

    BeamSearchConfig periodicConfig = config;
    periodicConfig.comparePeriodicReplanPlans = false;
    periodicConfig.macroReplanInterval = 4;
    if (!unlimited) {
        const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started).count();
        periodicConfig.timeLimitMs = std::max<std::int64_t>(
            0, config.timeLimitMs - used);
        if (periodicConfig.timeLimitMs <= config.safetyMarginMs) {
            primary.elapsedMs = used;
            return primary;
        }
    }
    BeamSearchResult periodic = Brain1::search(
        setting, matchState, previousScore, periodicConfig);

    auto officialScore = [](const BeamSearchResult& result)
        -> std::tuple<std::int64_t, std::int64_t, std::int64_t> {
        if (!result.success) {
            return {-1, -1, -1};
        }
        return {
            static_cast<std::int64_t>(countBits(
                result.finalState.score.matchBrands)),
            result.finalState.score.sumOfDailyBrandCounts,
            result.finalState.score.totalUdon};
    };
    const bool usePeriodic = periodic.success &&
        (!primary.success ||
         officialScore(periodic) > officialScore(primary) ||
         (officialScore(periodic) == officialScore(primary) &&
          periodic.evaluation.total > primary.evaluation.total));
    BeamSearchResult totalWork;
    totalWork.addSearchWork(primary);
    totalWork.addSearchWork(periodic);
    BeamSearchResult selected = usePeriodic
        ? std::move(periodic) : std::move(primary);
    selected.copySearchWork(totalWork);
    selected.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started).count();
    return selected;
}

// 固定幅の探索が締切より早く完了した場合は、幅を段階的に広げて
// 同じ日の探索をやり直す。重い問題では最初の pass 自体が締切まで走り、
// 軽い問題では余った時間をより広い探索へ自動的に振り向ける。
inline BeamSearchResult brain1Beam(
    const MatchSetting& setting,
    const MatchState& matchState,
    ScoreState previousScore = {},
    const BeamSearchConfig& config = {}) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();

    if (!config.iterativeDeepening) {
        auto result = brain1Pass(setting, matchState, previousScore, config);
        result.selectedBeamWidth = config.beamWidth;
        return result;
    }

    std::int64_t effectiveMs = std::numeric_limits<std::int64_t>::max();
    if (config.timeLimitMs > 0) {
        effectiveMs = std::max<std::int64_t>(
            0, config.timeLimitMs - config.safetyMarginMs);
    }
    if (config.useStateDeadline && matchState.endsAt > 0) {
        const auto end = std::chrono::system_clock::time_point{
            std::chrono::seconds(matchState.endsAt)};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::system_clock::now()).count();
        effectiveMs = std::min(
            effectiveMs,
            std::max<std::int64_t>(0, left - config.safetyMarginMs));
    }
    if (effectiveMs == std::numeric_limits<std::int64_t>::max()) {
        auto result = brain1Pass(setting, matchState, previousScore, config);
        result.selectedBeamWidth = config.beamWidth;
        return result;
    }
    if (effectiveMs <= 0) {
        auto result = brain1Pass(setting, matchState, previousScore, config);
        result.selectedBeamWidth = config.beamWidth;
        return result;
    }

    const auto deadline = started + std::chrono::milliseconds(effectiveMs);
    BeamSearchResult best;
    BeamSearchResult totalWork;
    using StabilitySignature = std::tuple<int, int, int, int, int, int, int, int>;
    StabilitySignature previousCompletedSignature{};
    bool haveCompletedSignature = false;
    std::size_t unchangedCompletedPasses = 0;

    const bool finalDay = matchState.day >= 0 &&
        static_cast<std::size_t>(matchState.day + 1) >= setting.dayCount();
    const bool tightFuelDay = detail::isFuelRiskDay(setting, matchState);
    const int futureFuelDivisor = tightFuelDay
        ? config.tightFuelFutureDivisor : config.futureFuelDivisor;
    std::vector<DijkstraTable> selectionCache(setting.field.cellCount());
    std::vector<bool> selectionCached(setting.field.cellCount(), false);
    auto resultScore = [&](const BeamSearchResult& result) {
        const auto future = finalDay ? detail::FutureRoutePotential{} :
            detail::futureRoutePotential(setting, matchState, result.finalState,
                selectionCache, selectionCached);
        return std::make_tuple(
            countBits(result.finalState.score.matchBrands),
            result.finalState.score.sumOfDailyBrandCounts,
            result.finalState.score.totalUdon,
            future.reachableBrands,
            -future.strandedPatrols, future.readyBrands, future.readyUdon,
            finalDay ? 0 : -future.distanceSum +
                future.patrolFuel / futureFuelDivisor);
    };
    auto better = [&](const BeamSearchResult& lhs,
                      const BeamSearchResult& rhs) {
        return lhs.success &&
            (!rhs.success || resultScore(lhs) > resultScore(rhs) ||
             (resultScore(lhs) == resultScore(rhs) &&
              lhs.evaluation.total > rhs.evaluation.total));
    };

    BeamSearchConfig pass = config;
    pass.useStateDeadline = false;
    pass.safetyMarginMs = 0;
    const std::size_t configuredBeamWidth = pass.beamWidth;
    const std::size_t configuredJointBeamWidth = pass.jointBeamWidth;
    // Automatically calibrated widths are upper targets for a long run, not
    // necessarily the best first width.  Start with a completed narrow pass
    // and retain its official-score result while deepening.  This avoids
    // spending the entire deadline in one oversized, unfinished pass.
    if (pass.autoSizedBeam && effectiveMs >= 10000 &&
        pass.beamWidth >= 4 * pass.minimumBeamWidth &&
        pass.jointBeamWidth >= 4 * pass.minimumJointBeamWidth) {
        pass.beamWidth = std::max(
            pass.minimumBeamWidth, pass.beamWidth / 4);
        pass.jointBeamWidth = std::max(
            pass.minimumJointBeamWidth, pass.jointBeamWidth / 4);
    }
    const std::size_t maxBeam = std::max<std::size_t>(
        configuredBeamWidth,
        std::min<std::size_t>(8192, configuredBeamWidth * 16));
    const std::size_t maxJoint = std::max<std::size_t>(
        configuredJointBeamWidth,
        std::min<std::size_t>(32768, configuredJointBeamWidth * 16));

    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (remaining <= 0) break;
        pass.timeLimitMs = remaining;
        BeamSearchResult current = brain1Pass(
            setting, matchState, previousScore, pass);
        current.selectedBeamWidth = pass.beamWidth;

        totalWork.addSearchWork(current);

        const bool timedOut = current.stopReason == SearchStopReason::TimeLimit;
        const bool completed = current.success && !timedOut;
        if (config.stopOnStableBeam && completed) {
            const auto signature = resultScore(current);
            if (haveCompletedSignature && signature == previousCompletedSignature) {
                ++unchangedCompletedPasses;
            } else {
                unchangedCompletedPasses = 0;
            }
            previousCompletedSignature = signature;
            haveCompletedSignature = true;
        }
        if (better(current, best)) best = std::move(current);
        if (timedOut) break;
        if (config.stopOnStableBeam && config.stableBeamPasses > 0 &&
            unchangedCompletedPasses >= config.stableBeamPasses) {
            best.stableBeamStopped = true;
            best.selectedBeamWidth = pass.beamWidth;
            break;
        }
        if ((config.quantityUpgrades || config.coordinatedRefinement) &&
            config.stopBeamAtBrandUpperBound && best.success &&
            (config.useSpotRouteLns || config.useUdonAlns) &&
            Clock::now() - started >= std::chrono::milliseconds(effectiveMs / 2)) {
            BrandMask all = 0;
            for (const auto& spot : setting.field.spots) all |= brandBit(spot.brandIndex);
            if ((best.finalState.dayBrands & all) == all) break;
        }

        const std::size_t nextBeam = std::min(
            maxBeam, pass.beamWidth > maxBeam / 2
                         ? maxBeam : pass.beamWidth * 2);
        const std::size_t nextJoint = std::min(
            maxJoint, pass.jointBeamWidth > maxJoint / 2
                          ? maxJoint : pass.jointBeamWidth * 2);
        const std::size_t spotLimit = std::max<std::size_t>(
            1, setting.field.spots.size());
        const std::size_t detourLimit = std::max<std::size_t>(
            spotLimit, setting.field.cellCount());
        const std::size_t nextMacro = std::min(
            spotLimit, pass.macroTargetLimit >= spotLimit / 2
                           ? spotLimit : pass.macroTargetLimit * 2);
        const std::size_t nextLookahead = std::min(
            spotLimit, pass.lookaheadCandidateLimit >= spotLimit / 2
                           ? spotLimit : pass.lookaheadCandidateLimit * 2);
        const std::size_t nextDetour = std::min(
            detourLimit, pass.detourCandidateLimit >= detourLimit / 2
                             ? detourLimit : pass.detourCandidateLimit * 2);
        const std::size_t nextFinalDetour = std::min(
            detourLimit,
            pass.finalDayDetourCandidateLimit >= detourLimit / 2
                ? detourLimit : pass.finalDayDetourCandidateLimit * 2);
        const bool alreadyExhaustive =
            nextBeam == pass.beamWidth &&
            nextJoint == pass.jointBeamWidth &&
            nextMacro == pass.macroTargetLimit &&
            nextLookahead == pass.lookaheadCandidateLimit &&
            nextDetour == pass.detourCandidateLimit &&
            nextFinalDetour == pass.finalDayDetourCandidateLimit &&
            pass.maxActionsPerAgent == 0;
        if (alreadyExhaustive) break;
        pass.beamWidth = nextBeam;
        pass.jointBeamWidth = nextJoint;
        pass.macroTargetLimit = nextMacro;
        pass.lookaheadCandidateLimit = nextLookahead;
        pass.detourCandidateLimit = nextDetour;
        pass.finalDayDetourCandidateLimit = nextFinalDetour;
        if (nextBeam == maxBeam && nextJoint == maxJoint) {
            pass.maxActionsPerAgent = 0;
            pass.adaptiveActionLimit = false;
        }
    }

    // どの pass も解を作れなかった場合も、従来どおり最低限の待機案を
    // 得るため元設定で一度だけ結果を構築する。
    if (!best.success) {
        BeamSearchConfig fallback = config;
        fallback.timeLimitMs = 1;
        fallback.safetyMarginMs = 1;
        fallback.useStateDeadline = false;
        best = brain1Pass(setting, matchState, previousScore, fallback);
        best.selectedBeamWidth = fallback.beamWidth;
        totalWork.addSearchWork(best);
    }
    best.copySearchWork(totalWork);
    best.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started).count();
    if (Clock::now() >= deadline) best.stopReason = SearchStopReason::TimeLimit;
    return best;
}

inline BeamSearchResult brain1(
    const MatchSetting& setting,
    const MatchState& matchState,
    ScoreState previousScore = {},
    const BeamSearchConfig& requestedConfig = {}) {
    using Clock = std::chrono::steady_clock;
    auto config = requestedConfig;
    if (!config.quantityUpgrades) {
        config.spotRouteLns.reflectTransit = false;
        config.routeColumns.maxSupplySchedules = 0;
        config.udonAlns.reallocateStock = false;
    }
    if (config.coordinatedRefinement) {
        config.udonAlns.reallocateStock = true;
        config.routeColumns.maxSupplySchedules = std::max<std::size_t>(
            config.routeColumns.maxSupplySchedules, 8);
    }
    if ((!config.refineBrands && !config.exactBrands) ||
        config.timeLimitMs < 0 || config.safetyMarginMs < 0)
        return brain1Beam(setting, matchState, previousScore, config);
    const auto started = Clock::now();
    std::int64_t budget = std::numeric_limits<std::int64_t>::max();
    if (config.timeLimitMs > 0) budget = config.timeLimitMs;
    if (config.useStateDeadline && matchState.endsAt > 0) {
        const auto end = std::chrono::system_clock::time_point{
            std::chrono::seconds(matchState.endsAt)};
        budget = std::min(budget,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end - std::chrono::system_clock::now()).count());
    }
    const bool limited = budget != std::numeric_limits<std::int64_t>::max();
    if (limited) budget = std::max<std::int64_t>(0, budget - config.safetyMarginMs);
    const bool routeColumnsActive = config.useSpotRouteLns &&
        config.routeColumns.enabled &&
        (!limited || budget >= config.routeColumns.minimumBudgetMs);
    const auto hardDeadline = limited ? started + std::chrono::milliseconds(budget)
                                  : Clock::time_point::max();
    const bool runDayBeam = config.useDayBeam && config.dayBeamWidth > 1 &&
        config.dayBeamDepth > 0 && matchState.day >= 0 &&
        static_cast<std::size_t>(matchState.day + 1) < setting.dayCount() &&
        (!limited || budget >= 5000);
    const auto dayBeamMs = runDayBeam ? (limited ? budget / 10 : 2000) : 0;
    const bool overnight = config.coordinatedRefinement && matchState.day >= 0 &&
        static_cast<std::size_t>(matchState.day + 1) < setting.dayCount();
    const auto overnightMs = overnight ? (limited ? std::min<std::int64_t>(500, budget / 30) : 500) : 0;
    const auto deadline = limited ? hardDeadline - std::chrono::milliseconds(dayBeamMs + overnightMs)
                                  : hardDeadline;
    const auto searchBudget = limited ? budget - dayBeamMs - overnightMs : budget;
    const bool finalDay = matchState.day >= 0 &&
        static_cast<std::size_t>(matchState.day + 1) >= setting.dayCount();
    const bool smallBoardTier = config.adaptiveSmallBoardBrandRescue &&
        setting.field.width == 16 && setting.field.height == 16;
    const bool severeFuel = detail::isFuelRiskDay(setting, matchState);
    const auto longSearch = detail::adaptiveLongSearchProfile(
        setting, config, searchBudget, finalDay, severeFuel);
    const auto quantityPercent = static_cast<std::int64_t>(
        longSearch.quantityBudgetPercent);
    std::int64_t beamBudgetPercent = 80;
    if (config.quantityUpgrades) {
        beamBudgetPercent = std::max<std::int64_t>(1, 90 - quantityPercent);
    } else if (config.autoSizedBeam) {
        if (smallBoardTier) {
            beamBudgetPercent = severeFuel
                ? std::max<std::int64_t>(85,
                      std::clamp<std::size_t>(
                          config.smallBoardBeamBudgetPercent, 50, 95))
                : config.adaptiveLongSearch
                      ? std::max<std::int64_t>(70, 100 - quantityPercent)
                      : std::clamp<std::size_t>(
                            config.smallBoardBeamBudgetPercent, 50, 95);
        } else {
            beamBudgetPercent = config.coordinatedRefinement
                ? severeFuel
                      ? std::max<std::int64_t>(85, 100 - quantityPercent)
                      : std::max<std::int64_t>(1, 100 - quantityPercent)
                : 95;
        }
    }
    const auto brandDeadline = limited && !config.exactBrands && config.quantityUpgrades
        ? started + std::chrono::milliseconds(searchBudget - searchBudget * quantityPercent / 100)
        : deadline;
    auto beamConfig = config;
    if (limited && budget > 1) {
        // In exact mode the beam is only an incumbent generator. Keep most of
        // the deadline for the exhaustive proof; otherwise exactBrands could
        // receive only 5--20% of the requested budget.
        beamConfig.timeLimitMs = config.exactBrands
            ? std::max<std::int64_t>(1, searchBudget / 5)
            : std::max<std::int64_t>(1,
                  searchBudget * beamBudgetPercent / 100);
        beamConfig.safetyMarginMs = 0;
        beamConfig.useStateDeadline = false;
        if ((config.quantityUpgrades || config.coordinatedRefinement) && config.autoSizedBeam) {
            // Daily widths were calibrated for the whole budget. Scale them
            // to the beam's actual slice so its first pass can still finish.
            const double share = static_cast<double>(beamConfig.timeLimitMs) /
                                 static_cast<double>(budget);
            beamConfig.beamWidth = std::max<std::size_t>(1,
                static_cast<std::size_t>(config.beamWidth * share));
            beamConfig.jointBeamWidth = std::max<std::size_t>(1,
                static_cast<std::size_t>(config.jointBeamWidth * share));
        }
    }
    auto result = brain1Beam(setting, matchState, previousScore, beamConfig);
    if (!result.success) return result;
    result.smallBoardBrandRescue = smallBoardTier;
    result.quantityBudgetPercentUsed = longSearch.quantityBudgetPercent;
    result.beamBudgetPercentUsed = config.exactBrands
        ? 20 : static_cast<std::size_t>(beamBudgetPercent);
    result.udonAlnsAttemptLimit = longSearch.udonAlns.maxAttempts;
    result.udonAlnsNodesPerAttempt = longSearch.udonAlns.nodesPerAttempt;
    // 安定幅はビームを広げる終了条件であって、系列最大性の証明ではない。
    // 担当2でもこの後の系列修復・系列最大化までは必ず通し、玉数専用の
    // 後段探索だけを省略して早期終了する。
    std::vector<DayPlanCandidate> futureCandidates;
    bool changed = false;
    const auto remember = [&] {
        if (!finalDay && (config.compareNextDayPlans || runDayBeam)) {
            if (std::none_of(futureCandidates.begin(), futureCandidates.end(),
                [&](const auto& old) { return old.state == result.finalState; }))
                futureCandidates.push_back({result.plan, result.finalState});
        }
    };
    remember();
    const bool runUdonAlns = config.useUdonAlns &&
        (!config.udonAlns.finalDayOnly || finalDay);
    const bool runSpotRouteLns = config.useSpotRouteLns &&
        (!config.spotRouteLns.finalDayOnly || finalDay);
    if (config.useBrandRepair && !config.exactBrands) {
        const auto repairConfig = strengthenedBrandRepair(
            setting, result.finalState, config.brandRepair);
        auto repairDeadline = brandDeadline;
        if (limited) {
            const auto now = Clock::now();
            const auto remaining =
                std::max(Clock::duration::zero(), brandDeadline - now);
            repairDeadline = now + (smallBoardTier
                ? remaining * 3 / 5
                : runUdonAlns ? remaining * 2 / 5 : remaining * 3 / 4);
        }
        const auto repair = repairDayBrands(setting, matchState, previousScore,
            result.plan, repairConfig, repairDeadline);
        result.brandRepairAttempts = repair.attempts;
        result.brandRepairExpanded = repair.expanded;
        bool acceptRepair = repair.improvements != 0;
        if (acceptRepair && smallBoardTier && !finalDay &&
            countBits(repair.finalState.score.matchBrands) ==
                countBits(result.finalState.score.matchBrands)) {
            std::vector<DijkstraTable> futureCache(setting.field.cellCount());
            std::vector<bool> futureCached(setting.field.cellCount(), false);
            const auto before = detail::futureRoutePotential(
                setting, matchState, result.finalState,
                futureCache, futureCached);
            const auto after = detail::futureRoutePotential(
                setting, matchState, repair.finalState,
                futureCache, futureCached);
            acceptRepair = after.reachableBrands >= before.reachableBrands &&
                after.strandedPatrols <= before.strandedPatrols;
        }
        result.brandRepairImprovements = acceptRepair ? repair.improvements : 0;
        if (acceptRepair) {
            result.plan = repair.plan;
            result.finalState = repair.finalState;
            changed = true;
            remember();
        }
    }
    // Establish the best reachable brand counts before optimizing quantity.
    // The quantity search below treats these counts as hard lower bounds, so a
    // larger udon total can never replace a plan with fewer brands.
    const auto brandNodeLimit = config.exactBrands ? std::size_t{0} :
        severeFuel
            ? (smallBoardTier
                  ? std::min<std::size_t>(
                        std::max(config.brandSearchNodeLimit,
                            config.smallBoardBrandSearchNodeLimit), 100000)
                  : std::min<std::size_t>(config.brandSearchNodeLimit, 10000))
            : smallBoardTier
                  ? std::max(config.brandSearchNodeLimit,
                        config.smallBoardBrandSearchNodeLimit)
                  : config.brandSearchNodeLimit;
    const auto refined = maximizeDayBrands(setting, matchState, previousScore,
        result.plan, brandNodeLimit,
        brandDeadline);
    result.brandOptimal = refined.optimal;
    result.matchBrandUpperBound = refined.matchUpperBound;
    result.dailyBrandUpperBound = refined.dailyUpperBound;
    result.brandSearchExpanded = refined.expanded;
    const auto brandScore = [](const GameState& state) {
        return std::make_pair(countBits(state.score.matchBrands), countBits(state.dayBrands));
    };
    if (brandScore(refined.finalState) > brandScore(result.finalState)) {
        result.plan = refined.plan;
        result.finalState = refined.finalState;
        changed = true;
        remember();
    }
    // A node cap is only a fast first pass. If its relaxed upper bound is
    // still open, spend the remaining brand-search slice proving or reaching
    // the maximum before any quantity-only refinement. The hard wall-clock
    // deadline remains authoritative, so this cannot delay submission.
    if (limited && !result.brandOptimal && Clock::now() < brandDeadline &&
        brandScore(result.finalState) < std::make_pair(
            result.matchBrandUpperBound, result.dailyBrandUpperBound)) {
        const auto rescue = maximizeDayBrands(
            setting, matchState, previousScore, result.plan,
            0, brandDeadline);
        result.brandSearchExpanded += rescue.expanded;
        result.brandOptimal = rescue.optimal;
        result.matchBrandUpperBound = rescue.matchUpperBound;
        result.dailyBrandUpperBound = rescue.dailyUpperBound;
        if (brandScore(rescue.finalState) > brandScore(result.finalState)) {
            result.plan = rescue.plan;
            result.finalState = rescue.finalState;
            changed = true;
            remember();
        }
    }
    if (result.stableBeamStopped) {
        if (changed) {
            EvaluationContext context;
            context.scoreAtDayStart = previousScore;
            result.evaluation = evaluateGameState(
                result.finalState, context, config.weights);
        }
        const auto finalReplay = simulateDay(
            setting, matchState, result.plan, previousScore);
        if (finalReplay.valid) {
            result.supplyUtilization = finalReplay.supplyUtilization;
        }
        result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started).count();
        if (limited && Clock::now() >= brandDeadline && !result.brandOptimal) {
            result.stopReason = SearchStopReason::TimeLimit;
        }
        return result;
    }
    if (runSpotRouteLns && Clock::now() < deadline) {
        auto routeDeadline = deadline;
        if (runUdonAlns && limited) {
            const auto now = Clock::now();
            routeDeadline = now +
                std::max(Clock::duration::zero(), deadline - now) *
                static_cast<std::int64_t>(
                    100 - longSearch.alnsPercentOfRefinement) / 100;
        }
        // Columns get their own share even if team LNS exhausts its budget.
        const auto legacyRouteDeadline = (config.quantityUpgrades || config.coordinatedRefinement) && limited && routeColumnsActive
            ? Clock::now() + std::max(Clock::duration::zero(),
                routeDeadline - Clock::now()) * 2 / 3 : routeDeadline;
        if (config.spotRouteLns.spotPortfolio && Clock::now() < legacyRouteDeadline) {
            const auto now = Clock::now();
            const auto portfolioDeadline = now +
                std::max(Clock::duration::zero(), legacyRouteDeadline - now) / 2;
            const std::array<int, 4> hardnessWeights{{4, 0, 16, 8}};
            const auto boardBrandCount = static_cast<std::uint32_t>(
                setting.field.brands.size());
            const bool quantityFirstBoard =
                config.smallBoardQuantityFirstRoutes &&
                setting.field.width == 16 && setting.field.height == 16 &&
                boardBrandCount == 14 && setting.dayCount() == 8;
            const bool quantityFirst = quantityFirstBoard;
            const std::size_t passCount = hardnessWeights.size() +
                static_cast<std::size_t>(quantityFirst);
            for (std::size_t pass = 0;
                 pass < passCount && Clock::now() < portfolioDeadline;
                 ++pass) {
                auto portfolioConfig = config.spotRouteLns;
                const bool quantityPass = quantityFirst && pass == 0;
                const std::size_t rankedPass = quantityPass ? 0 : pass -
                    static_cast<std::size_t>(quantityFirst);
                portfolioConfig.quantityFirstRanking = quantityPass;
                portfolioConfig.requiredMatchBrandCount = quantityPass
                    ? boardBrandCount : 0;
                portfolioConfig.requiredDayBrandCount = quantityPass
                    ? boardBrandCount : 0;
                if (quantityPass) {
                    portfolioConfig.allowFuturePositionChange = finalDay;
                    portfolioConfig.preserveIntermediateFuelExactly = true;
                }
                // Keep the established fast proposal as well as the richer
                // transit/refuel model: their finite beams explore different routes.
                portfolioConfig.reflectTransit = quantityPass || rankedPass != 0
                    ? config.spotRouteLns.reflectTransit : false;
                portfolioConfig.spotPortfolioHardnessWeight = quantityPass
                    ? 0 : hardnessWeights[rankedPass];
                // Preserve the established route rankings in the first two
                // passes. The later, wider passes diversify toward brands
                // that only a few patrols can still reach today.
                portfolioConfig.spotPortfolioReachabilityRarity =
                    config.spotRouteLns.spotPortfolioReachabilityRarity &&
                    !quantityPass && rankedPass >= 2;
                const auto maxPortfolioWidth = setting.field.cellCount() >= 1024
                    ? std::size_t{512} : std::size_t{2048};
                portfolioConfig.spotPortfolioBeamWidth = std::min<std::size_t>(
                    maxPortfolioWidth,
                    portfolioConfig.spotPortfolioBeamWidth >
                            (maxPortfolioWidth >> std::min<std::size_t>(rankedPass, 3))
                        ? maxPortfolioWidth
                        : portfolioConfig.spotPortfolioBeamWidth <<
                            std::min<std::size_t>(rankedPass, 3));
                portfolioConfig.spotPortfolioCandidatesPerDepth =
                    std::min<std::size_t>(64,
                        portfolioConfig.spotPortfolioCandidatesPerDepth >
                                (std::size_t{64} >>
                                    std::min<std::size_t>(rankedPass, 3))
                            ? std::size_t{64}
                            : portfolioConfig.spotPortfolioCandidatesPerDepth <<
                                std::min<std::size_t>(rankedPass, 3));
                const auto passDeadline = quantityPass && limited
                    ? Clock::now() + std::max(Clock::duration::zero(),
                        portfolioDeadline - Clock::now()) / 3
                    : portfolioDeadline;
                const auto portfolio = searchSpotPortfolio(setting, matchState,
                    previousScore, result.plan, portfolioConfig,
                    passDeadline);
                result.spotRouteLnsAttempts += portfolio.attempts;
                result.spotRouteLnsImprovements += portfolio.improvements;
                result.spotPortfolioMaxBrands = std::max(
                    result.spotPortfolioMaxBrands, portfolio.portfolioMaxBrands);
                result.spotPortfolioMaxUdon = std::max(
                    result.spotPortfolioMaxUdon, portfolio.portfolioMaxUdon);
                result.spotPortfolioMaxValidUdon = std::max(
                    result.spotPortfolioMaxValidUdon,
                    portfolio.portfolioMaxValidUdon);
                if (portfolio.improvements) {
                    result.plan = portfolio.plan;
                    result.finalState = portfolio.finalState;
                    changed = true;
                    remember();
                }
                // Keep the established brand-first passes as independent
                // neighborhoods. The quantity pass contributes a candidate
                // but never suppresses a later route that scores better.
            }
        }
        auto localDeadline = legacyRouteDeadline;
        if (config.spotRouteLns.teamNeighborhood && limited) {
            const auto now = Clock::now();
            localDeadline = now +
                std::max(Clock::duration::zero(), legacyRouteDeadline - now) / 3;
        }
        const auto routed = improveSpotRoutes(setting, matchState, previousScore,
            result.plan, config.spotRouteLns, localDeadline);
        result.spotRouteLnsAttempts += routed.attempts;
        result.spotRouteLnsImprovements += routed.improvements;
        if (routed.improvements) {
            result.plan = routed.plan;
            result.finalState = routed.finalState;
            changed = true;
            remember();
        }
        // Use only time left by the established local search.  Running this
        // first reduced its node count under short wall-clock limits.
        if (config.spotRouteLns.terminalWaitFill &&
            (!limited || budget >=
                config.spotRouteLns.terminalFillMinimumBudgetMs) &&
            Clock::now() < localDeadline) {
            const auto filled = fillTerminalPatrolWaits(setting, matchState,
                previousScore, result.plan, config.spotRouteLns,
                localDeadline);
            result.spotRouteLnsAttempts += filled.attempts;
            result.spotRouteLnsImprovements += filled.improvements;
            if (filled.improvements) {
                result.plan = filled.plan;
                result.finalState = filled.finalState;
                changed = true;
                remember();
            }
        }
        if (config.spotRouteLns.teamNeighborhood && Clock::now() < legacyRouteDeadline) {
            auto teamConfig = config.spotRouteLns;
            if (limited && legacyRouteDeadline - Clock::now() > std::chrono::seconds(3))
                teamConfig.maxTeamAttempts = std::max<std::size_t>(
                    teamConfig.maxTeamAttempts, 2000);
            const auto team = improveTeamSpotRoutes(setting, matchState,
                previousScore, result.plan, teamConfig, legacyRouteDeadline);
            result.spotRouteLnsAttempts += team.attempts;
            result.spotRouteLnsImprovements += team.improvements;
            if (team.improvements) {
                result.plan = team.plan;
                result.finalState = team.finalState;
                changed = true;
                remember();
            }
        }
        if (routeColumnsActive && Clock::now() < routeDeadline) {
            const auto columns = searchRouteColumns(setting, matchState,
                previousScore, result.plan, config.spotRouteLns,
                config.routeColumns, routeDeadline);
            result.routeColumnsGenerated = columns.generated;
            result.routeColumnsMasterNodes = columns.masterNodes;
            result.routeColumnsImprovements = columns.improvements;
            if (columns.improvements) {
                result.plan = columns.plan;
                result.finalState = columns.finalState;
                changed = true;
                remember();
            }
        }
    }
    if (runUdonAlns) {
        const auto refineUdon = [&](const DailyPlan& plan,
                                    const UdonAlnsConfig& alnsConfig,
                                    Clock::time_point alnsDeadline) {
            if (config.udonRefiner) {
                return config.udonRefiner(setting, matchState, previousScore,
                    plan, alnsConfig, alnsDeadline);
            }
            return improveDayUdon(setting, matchState, previousScore,
                plan, alnsConfig, alnsDeadline);
        };
        const bool reserveExpanded = limited && finalDay &&
            longSearch.udonAlns.expandedPortfolio &&
            (config.quantityUpgrades || searchBudget >= 30000);
        const auto normalDeadline = reserveExpanded
            ? Clock::now() + std::max(Clock::duration::zero(),
                deadline - Clock::now()) * 2 / 3 : deadline;
        auto improved = refineUdon(
            result.plan, longSearch.udonAlns, normalDeadline);
        const bool reachedBrandBound =
            countBits(improved.finalState.score.matchBrands) >=
                result.matchBrandUpperBound &&
            countBits(improved.finalState.dayBrands) >=
                result.dailyBrandUpperBound;
        // Large-board leaders already saturate both brand objectives. Use any
        // time left by the bounded normal ALNS for an independent, wider
        // neighborhood. Both passes start from the same incumbent and the
        // official lexicographic score chooses the returned plan.
        const bool expandedQuantityPortfolio = longSearch.udonAlns.expandedPortfolio &&
            (finalDay || (setting.field.width >= 24 && reachedBrandBound));
        if (expandedQuantityPortfolio && !improved.stagnationStopped &&
            Clock::now() < deadline) {
            auto expandedConfig = longSearch.udonAlns;
            expandedConfig.maxAttempts = std::max<std::size_t>(
                expandedConfig.maxAttempts, 16);
            expandedConfig.nodesPerAttempt = std::max<std::size_t>(
                expandedConfig.nodesPerAttempt, 1600);
            expandedConfig.maxPatrols = std::max<std::size_t>(
                expandedConfig.maxPatrols, 3);
            // Problem instances favor different SA neighborhoods.  Make the
            // independent pass explore the other distribution and RNG stream;
            // the official lexicographic score still selects the result.
            expandedConfig.arenaGuidedNeighborhoods =
                !longSearch.udonAlns.arenaGuidedNeighborhoods;
            expandedConfig.randomSeed ^=
                UINT64_C(0xd1b54a32d192ed03);
            expandedConfig.annealingWarmStart =
                longSearch.udonAlns.warmStartPortfolio;
            // A proxy for tomorrow is not strong enough to justify changing
            // the endpoint in the extra quantity pass. Intermediate-day
            // alternatives must finish with exactly the incumbent positions
            // and fuel; the final day has no downstream state to protect.
            expandedConfig.allowFuturePositionChange = finalDay;
            expandedConfig.expandedPortfolio = false;
            const auto expanded = refineUdon(
                result.plan, expandedConfig, deadline);
            improved.attempts += expanded.attempts;
            improved.expanded += expanded.expanded;
            improved.improvements += expanded.improvements;
            improved.acceptedMoves += expanded.acceptedMoves;
            improved.acceptedWorseMoves += expanded.acceptedWorseMoves;
            improved.greedyAttempts += expanded.greedyAttempts;
            improved.jointDestroyAttempts += expanded.jointDestroyAttempts;
            improved.jointDestroyExpanded += expanded.jointDestroyExpanded;
            improved.jointDestroyImprovements +=
                expanded.jointDestroyImprovements;
            for (std::size_t i = 0;
                 i < improved.neighborhoodAttempts.size(); ++i) {
                improved.neighborhoodAttempts[i] +=
                    expanded.neighborhoodAttempts[i];
                improved.neighborhoodAccepted[i] +=
                    expanded.neighborhoodAccepted[i];
            }
            improved.interrupted = improved.interrupted || expanded.interrupted;
            improved.stagnationStopped =
                improved.stagnationStopped || expanded.stagnationStopped;
            const auto score = [](const GameState& state) {
                return std::make_tuple(
                    countBits(state.score.matchBrands),
                    countBits(state.dayBrands),
                    state.score.totalUdon);
            };
            if (score(expanded.finalState) > score(improved.finalState)) {
                improved.plan = expanded.plan;
                improved.finalState = expanded.finalState;
            }
        }
        result.udonAlnsAttempts = improved.attempts;
        result.udonAlnsExpanded = improved.expanded;
        result.udonAlnsImprovements = improved.improvements;
        result.udonAlnsAcceptedMoves = improved.acceptedMoves;
        result.udonAlnsAcceptedWorseMoves = improved.acceptedWorseMoves;
        result.udonAlnsGreedyAttempts = improved.greedyAttempts;
        result.udonJointDestroyAttempts = improved.jointDestroyAttempts;
        result.udonJointDestroyExpanded = improved.jointDestroyExpanded;
        result.udonJointDestroyImprovements =
            improved.jointDestroyImprovements;
        result.udonAlnsNeighborhoodAttempts = improved.neighborhoodAttempts;
        result.udonAlnsNeighborhoodAccepted = improved.neighborhoodAccepted;
        result.udonSaStagnationStopped = improved.stagnationStopped;
        if (improved.improvements) {
            result.plan = improved.plan;
            result.finalState = improved.finalState;
            changed = true;
            remember();
        }
    }
    // 全日で広げる案は翌日の系列とSA乱数列に退行があった。
    // 既存の最終日探索が完了した後、残時間だけで独立候補を比較する。
    if (config.smallBoardFinalColumns && routeColumnsActive && finalDay &&
        setting.field.width == 16 && setting.field.height == 16 &&
        countBits(result.finalState.score.matchBrands) >= result.matchBrandUpperBound &&
        countBits(result.finalState.dayBrands) >= result.dailyBrandUpperBound &&
        Clock::now() < deadline) {
        auto columnConfig = config.routeColumns;
        columnConfig.labelWidth = std::max<std::size_t>(columnConfig.labelWidth, 48);
        columnConfig.columnsPerAgent = std::max<std::size_t>(columnConfig.columnsPerAgent, 48);
        columnConfig.maxMasterNodes = std::max<std::size_t>(columnConfig.maxMasterNodes, 250000);
        const auto wide = searchRouteColumns(setting, matchState,
            previousScore, result.plan, config.spotRouteLns, columnConfig,
            deadline);
        result.routeColumnsGenerated += wide.generated;
        result.routeColumnsMasterNodes += wide.masterNodes;
        result.routeColumnsImprovements += wide.improvements;
        if (wide.improvements) {
            result.plan = wide.plan;
            result.finalState = wide.finalState;
            changed = true;
        }
    }
    if (runDayBeam && Clock::now() < hardDeadline) {
        const auto futureDeadline = limited ? hardDeadline - std::chrono::milliseconds(overnightMs) :
            Clock::now() + std::chrono::milliseconds(dayBeamMs);
        auto probe = config;
        probe.refineBrands = false;
        probe.exactBrands = false;
        probe.useDayBeam = false;
        probe.compareNextDayPlans = false;
        probe.comparePeriodicReplanPlans = false;
        probe.iterativeDeepening = false;
        probe.autoTuneWeights = false;
        probe.useStateDeadline = false;
        probe.safetyMarginMs = 0;
        probe.threadCount = 1;
        probe.beamWidth = 16;
        probe.jointBeamWidth = 64;
        const auto runProbe = [&](const MatchState& state, ScoreState score, int variant,
                                  Clock::time_point end) {
            auto p = probe;
            if (variant == 1) {
                p.weights.nextDayBrand *= 4;
                p.weights.nextDayUdon *= 4;
                p.weights.nextDayBrandApproach *= 4;
            } else if (variant == 2) {
                p.weights.remainingFuel *= 100;
                p.weights.fuelRisk *= 4;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            if (left <= 0) return BeamSearchResult{};
            p.timeLimitMs = left;
            return Brain1::search(setting, state, score, p);
        };
        const auto generationDeadline = Clock::now() + (futureDeadline - Clock::now()) / 4;
        for (int variant = 0; variant < 3 && Clock::now() < generationDeadline; ++variant) {
            const auto end = Clock::now() + (generationDeadline - Clock::now()) / (3 - variant);
            auto alternative = runProbe(matchState, previousScore, variant, end);
            if (alternative.success && brandScore(alternative.finalState) == brandScore(result.finalState))
                futureCandidates.push_back({std::move(alternative.plan), alternative.finalState});
        }
        // The immediate winner must remain a root even when no refiner improved.
        remember();
        futureCandidates.erase(std::remove_if(futureCandidates.begin(), futureCandidates.end(),
            [&](const auto& c) {
                return brandScore(c.state) != brandScore(result.finalState) ||
                    static_cast<std::int64_t>(c.state.score.totalUdon) +
                        std::max(0, config.dayBeamMaxUdonLoss) < result.finalState.score.totalUdon;
            }),
            futureCandidates.end());
        retainDayCandidates(futureCandidates, std::clamp<std::size_t>(config.dayBeamWidth, 2, 8),
                            setting, matchState);
        result.dayBeamCandidates = futureCandidates.size();
        if (futureCandidates.size() > 1) {
            const auto future = searchFutureDays(setting, matchState, futureCandidates,
                std::clamp<std::size_t>(config.dayBeamWidth, 2, 8),
                std::min<std::size_t>(config.dayBeamDepth, 3), futureDeadline, runProbe);
            result.dayBeamExpanded = future.expanded;
            result.dayBeamComplete = future.complete;
            if (future.complete) {
                result.plan = futureCandidates[future.selected].plan;
                result.finalState = futureCandidates[future.selected].state;
                changed = true;
            }
        }
    }
    if (!runDayBeam && !finalDay && config.compareNextDayPlans &&
        futureCandidates.size() > 1 &&
        deadline - Clock::now() > std::chrono::milliseconds(
            500 + 2 * std::max<std::int64_t>(1, config.nextDayProbeMs) *
                static_cast<std::int64_t>(futureCandidates.size()))) {
        const auto currentBrandScore = brandScore(result.finalState);
        auto probeConfig = config;
        probeConfig.refineBrands = false;
        probeConfig.exactBrands = false;
        probeConfig.useBrandRepair = false;
        probeConfig.useSpotRouteLns = false;
        probeConfig.useUdonAlns = false;
        probeConfig.iterativeDeepening = false;
        probeConfig.comparePeriodicReplanPlans = false;
        probeConfig.compareNextDayPlans = false;
        probeConfig.useStateDeadline = false;
        probeConfig.safetyMarginMs = 0;
        probeConfig.threadCount = 1;
        probeConfig.beamWidth = 16;
        probeConfig.jointBeamWidth = 64;
        probeConfig.timeLimitMs = std::max<std::int64_t>(
            1, config.nextDayProbeMs);
        bool haveProbe = false;
        std::tuple<int, int, int, int> bestProbe{};
        std::size_t bestIndex = 0;
        for (std::size_t i = 0; i < futureCandidates.size(); ++i) {
            if (deadline - Clock::now() <= std::chrono::milliseconds(500)) break;
            const auto& candidate = futureCandidates[i];
            if (brandScore(candidate.state) != currentBrandScore) continue;
            MatchState tomorrow;
            tomorrow.day = matchState.day + 1;
            tomorrow.trafficByPos = matchState.trafficByPos;
            tomorrow.agents.assign(candidate.state.agents.begin(),
                candidate.state.agents.begin() + candidate.state.agentCount);
            int worstBrands = std::numeric_limits<int>::max();
            int worstUdon = std::numeric_limits<int>::max();
            for (int scenario = 0; scenario < 2; ++scenario) {
                if (deadline - Clock::now() <= std::chrono::milliseconds(500)) {
                    worstBrands = -1;
                    break;
                }
                if (scenario == 1)
                    tomorrow.trafficByPos.assign(setting.field.cellCount(),
                        TrafficStatus::Smooth);
                const auto trial = Brain1::search(setting, tomorrow,
                    candidate.state.score, probeConfig);
                if (!trial.success) { worstBrands = -1; break; }
                worstBrands = std::min(worstBrands,
                    static_cast<int>(countBits(trial.finalState.dayBrands)));
                worstUdon = std::min(worstUdon,
                    trial.finalState.dayUdon);
            }
            if (worstBrands < 0) continue;
            const auto projected = std::make_tuple(
                static_cast<int>(countBits(candidate.state.score.matchBrands)),
                worstBrands,
                candidate.state.score.totalUdon + worstUdon,
                candidate.state.score.totalUdon);
            if (!haveProbe || projected > bestProbe) {
                haveProbe = true;
                bestProbe = projected;
                bestIndex = i;
            }
        }
        if (haveProbe) {
            result.plan = std::move(futureCandidates[bestIndex].plan);
            result.finalState = futureCandidates[bestIndex].state;
            changed = true;
        }
    }
    if (overnight && Clock::now() < hardDeadline) {
        const auto tailDeadline = limited ? hardDeadline : Clock::now() + std::chrono::milliseconds(overnightMs);
        const auto prepared = improveOvernightPosition(
            setting, matchState, previousScore, result.plan, tailDeadline);
        result.overnightAttempts = prepared.attempts;
        result.overnightImprovements = prepared.improvements;
        if (prepared.improvements) {
            result.plan = prepared.plan;
            result.finalState = prepared.state;
            changed = true;
        }
    }
    if (changed) {
        EvaluationContext context;
        context.scoreAtDayStart = previousScore;
        result.evaluation = evaluateGameState(result.finalState, context, config.weights);
    }
    // Refiners can replace the beam plan after its utilization was measured.
    // Replay the final winner so diagnostics always describe the submitted
    // plan rather than an earlier incumbent.
    const auto finalReplay = simulateDay(
        setting, matchState, result.plan, previousScore);
    if (finalReplay.valid) {
        result.supplyUtilization = finalReplay.supplyUtilization;
    }
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started).count();
    if (limited && Clock::now() >= deadline && !result.brandOptimal)
        result.stopReason = SearchStopReason::TimeLimit;
    return result;
}

}  // namespace benzen
