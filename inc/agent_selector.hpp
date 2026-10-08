#pragma once

#include "brain/brain0.hpp"
#include "brain/brain1.hpp"
#include "trial.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace benzen {

struct AgentSelectorConfig {
    // 既定では燃料切れで全車が行動不能になるのを避けるため、
    // 補給車を最低1台残す。-1なら巡回車を補給車以上残す範囲で
    // 自動決定する。必要なら明示値で半数を超える補給車も許可する。
    std::int32_t minSupplyCount = 1;
    std::int32_t maxSupplyCount = -1;

    // 距離評価後にbrain1で試走する候補数。0なら距離評価だけを使う。
    std::size_t trialCandidateLimit = 12;
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
    // 期限付き試走でも trialCandidateLimit 件は公平に比較し、
    // 基本候補の完走後に残り時間があれば未試走配置も追加する。
    bool extendTrialsToDeadline = false;
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
    std::size_t completedTrialScenarios = 0;
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
        std::int32_t reachableBrandCount = 0;
        std::int64_t reachableDailyBrandCount = 0;
        std::int64_t theoreticalMatchUdon = 0;
    };

    static std::int32_t maximumSupplyCount(
        std::size_t agentCount,
        const AgentSelectorConfig& cfg) noexcept {
        return cfg.maxSupplyCount < 0
                   ? static_cast<std::int32_t>(agentCount / 2)
                   : cfg.maxSupplyCount;
    }

    // 公開試合1539～1671の上位結果から推定した中心構成は、
    // 4台で3巡回+1補給、5台で4+1、7台で5+2。特に5台盤面では
    // 補給2台だと在庫上限だけで上位の玉数へ届かない試合がある。
    // 個別の盤面評価で前後の台数へ動かせるよう、これは候補順の中心に
    // だけ使い、最終決定は距離評価と試走比較に委ねる。
    // std::clampを使わず、C++標準設定が古い補完環境でも解析できる形にする。
    static std::int32_t preferredSupplyCount(
        std::size_t agentCount, const AgentSelectorConfig& cfg,
        std::int32_t maxSupply) noexcept {
        const std::int32_t center = agentCount == 5
            ? 1
            : static_cast<std::int32_t>((agentCount + 1) / 3);
        return std::max(cfg.minSupplyCount, std::min(center, maxSupply));
    }

    static std::vector<std::int32_t> supplyCountOrder(
        std::size_t agentCount, const AgentSelectorConfig& cfg,
        std::int32_t maxSupply) {
        std::vector<std::int32_t> order;
        const std::int32_t preferred =
            preferredSupplyCount(agentCount, cfg, maxSupply);
        const std::size_t count = static_cast<std::size_t>(
            maxSupply - cfg.minSupplyCount + 1);
        order.reserve(count);
        order.push_back(preferred);
        for (std::int32_t offset = 1;
             order.size() < count; ++offset) {
            const std::int32_t lower = preferred - offset;
            const std::int32_t upper = preferred + offset;
            if (lower >= cfg.minSupplyCount) order.push_back(lower);
            if (upper <= maxSupply) order.push_back(upper);
        }
        return order;
    }

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

        const std::int32_t maximum = maximumSupplyCount(n, cfg);
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

    static std::int32_t geometricDistance(
        const Field& field, CellPos lhs, CellPos rhs) noexcept {
        const Coord a = field.toCoord(lhs);
        const Coord b = field.toCoord(rhs);
        if (!field.contains(a) || !field.contains(b)) return kImpassableCost;

        // 行の偶奇で半マスずれる盤面を、横座標を2倍した座標へ写す。
        const std::int32_t ax = 2 * a.x + ((a.y & 1) == 0 ? 1 : 0);
        const std::int32_t bx = 2 * b.x + ((b.y & 1) == 0 ? 1 : 0);
        return std::abs(ax - bx) + 2 * std::abs(a.y - b.y);
    }

    // Dijkstra 中に締切を迎えても、添字順で補給車を決めない。
    // スポットと他の開始位置への近さを使う軽量な k-median 評価なら、
    // 探索時間をほぼ使わず中央・広域配置の補給車を選べる。
    static std::vector<AgentKind> fallbackKinds(
        const MatchSetting& s,
        const AgentSelectorConfig& cfg,
        std::int32_t maxSupply) {
        const std::size_t n = s.agentCount();
        const std::int32_t supplyN =
            preferredSupplyCount(n, cfg, maxSupply);
        const std::uint32_t masks =
            std::uint32_t{1} << static_cast<std::uint32_t>(n);
        std::uint32_t bestMask = 0;
        std::int64_t bestCost = std::numeric_limits<std::int64_t>::max();
        for (std::uint32_t mask = 0; mask < masks; ++mask) {
            if (countBits(mask) != supplyN) continue;
            std::int64_t cost = 0;
            for (const Spot& spot : s.field.spots) {
                std::int32_t near = kImpassableCost;
                for (std::size_t q = 0; q < n; ++q) {
                    if ((mask & (std::uint32_t{1} << q)) == 0) continue;
                    near = std::min(near, geometricDistance(
                        s.field, s.agentStarts[q], spot.pos));
                }
                cost += near;
            }
            for (std::size_t p = 0; p < n; ++p) {
                if ((mask & (std::uint32_t{1} << p)) != 0) continue;
                std::int32_t near = kImpassableCost;
                for (std::size_t q = 0; q < n; ++q) {
                    if ((mask & (std::uint32_t{1} << q)) == 0) continue;
                    near = std::min(near, geometricDistance(
                        s.field, s.agentStarts[q], s.agentStarts[p]));
                }
                cost += near;
            }
            if (cost < bestCost || (cost == bestCost && mask < bestMask)) {
                bestCost = cost;
                bestMask = mask;
            }
        }

        std::vector<AgentKind> kinds(n, AgentKind::Patrol);
        for (std::size_t i = 0; i < n; ++i) {
            if ((bestMask & (std::uint32_t{1} << i)) != 0)
                kinds[i] = AgentKind::Supply;
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
            return limited && std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count() <= 0;
        };

        const std::size_t n = s.agentCount();
        const std::int32_t maxSupply = maximumSupplyCount(n, cfg);
        const std::int32_t maxDaySteps = *std::max_element(
            s.daySteps.begin(), s.daySteps.end());
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
                fallback.kinds = fallbackKinds(s, cfg, maxSupply);
                fallback.supplyCount = static_cast<std::int32_t>(std::count(
                    fallback.kinds.begin(), fallback.kinds.end(),
                    AgentKind::Supply));
                return fallback;
            }
            dist.push_back(Brain0::dijkstra(
                s, base, start, [&]() { return expired(); }));
        }

        // 開始位置ごとの巡回能力は種別配置に依存しない。
        std::array<double, kMaxAgents> patrolCapacities{};
        for (std::size_t p = 0; p < n; ++p) {
            std::array<double, 3> nearest{};
            for (const Spot& spot : s.field.spots) {
                const std::int32_t d = distanceTo(dist[p], spot.pos);
                if (d > maxDaySteps || d >= kImpassableCost) continue;
                double value = 0.0;
                for (const std::int32_t steps : s.daySteps) {
                    if (d <= steps) {
                        value += static_cast<double>(steps + 1 - d) /
                                 static_cast<double>(steps + 1);
                    }
                }
                value /= static_cast<double>(s.dayCount());
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
            patrolCapacities[p] = nearest[0] +
                0.5 * nearest[1] + 0.25 * nearest[2];
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
            std::vector<std::vector<bool>> dailyBrands(
                s.dayCount(), std::vector<bool>(s.field.brands.size(), false));
            std::vector<std::int32_t> dailyStockCaps(s.dayCount(), 0);
            std::int32_t spots = 0;
            std::int64_t distSum = 0;
            const std::int32_t patrolN =
                static_cast<std::int32_t>(n) - supplyN;
            for (const Spot& spot : s.field.spots) {
                std::int32_t near = kImpassableCost;
                for (std::size_t i = 0; i < n; ++i) {
                    if (c.kinds[i] != AgentKind::Patrol) {
                        continue;
                    }
                    near = std::min(near, distanceTo(dist[i], spot.pos));
                }
                if (near > maxDaySteps || near >= kImpassableCost) {
                    continue;
                }
                ++spots;
                if (spot.brandIndex >= 0 &&
                    static_cast<std::size_t>(spot.brandIndex) <
                        brands.size()) {
                    brands[static_cast<std::size_t>(spot.brandIndex)] = true;
                }
                for (std::size_t day = 0; day < s.dayCount(); ++day) {
                    const std::int32_t steps = s.daySteps[day];
                    if (near > steps) continue;
                    // 同じ巡回車は同じスポットから1日に一度だけ取得できる。
                    dailyStockCaps[day] += std::min(spot.maxStock, patrolN);
                    distSum += near;
                    if (spot.brandIndex >= 0 &&
                        static_cast<std::size_t>(spot.brandIndex) <
                            brands.size()) {
                        dailyBrands[day][static_cast<std::size_t>(
                            spot.brandIndex)] = true;
                    }
                }
            }

            const auto brandN = static_cast<std::int32_t>(
                std::count(brands.begin(), brands.end(), true));
            c.reachableBrandCount = brandN;
            for (std::size_t day = 0; day < s.dayCount(); ++day) {
                c.reachableDailyBrandCount += static_cast<std::int64_t>(
                    std::count(dailyBrands[day].begin(),
                               dailyBrands[day].end(), true));
                c.theoreticalMatchUdon += std::min<std::int64_t>(
                    dailyStockCaps[day],
                    static_cast<std::int64_t>(patrolN) * s.daySteps[day]);
            }

            // 到達可能なスポットの和集合だけでは、巡回車1台で全地点に
            // 届く配置と、複数台で並行して回れる配置を区別できない。
            // 各巡回車が初日に担当できそうな近い3地点を見積もる。
            double patrolCapacity = 0.0;
            for (std::size_t p = 0; p < n; ++p) {
                if (c.kinds[p] != AgentKind::Patrol) continue;
                patrolCapacity += patrolCapacities[p];
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
                3.0 * static_cast<double>(std::max(1, maxDaySteps));
            const double fuelPressure = std::max(
                0.0,
                (maxFuel - s.fuelLimit) / maxFuel);
            // 公開上位結果で安定していた中心構成を弱い事前分布として使う。
            // ブランド到達性は決して上書きせず、同等の候補間だけで補給不足を
            // 避ける程度の重みにする。玉数上限の差が大きければ巡回寄りも選べる。
            const std::int32_t preferredSupply =
                preferredSupplyCount(n, cfg, maxSupply);
            // 長期戦では初日の取得上限より、燃料切れを翌日以降へ
            // 連鎖させない構成の価値が高い。試走を省略する全自動でも
            // 7台の中心構成（巡回5＋補給2）が僅差の在庫上限に負けない
            // よう、日数に応じて弱い事前分布を最大2倍まで強める。
            const double longMatchFactor = 1.0 + std::min(
                1.0,
                std::max(0.0,
                    (static_cast<double>(s.dayCount()) - 4.0) / 6.0));
            const double compositionPrior =
                -5.0e5 * longMatchFactor * (1.0 + fuelPressure) *
                std::abs(supplyN - preferredSupply);
            c.heuristicScore =
                // 最大10日・32系列・8台・32スポットでも、上位の評価軸
                // 1点を下位評価軸の総和が逆転しない大きさにする。
                2.0e10 * brandN +
                5.0e7 * static_cast<double>(c.reachableDailyBrandCount) +
                1.0e6 * spots +
                1.0e4 * static_cast<double>(c.theoreticalMatchUdon) -
                100.0 * static_cast<double>(distSum) -
                10.0 * fuelPressure * static_cast<double>(supplyDist) +
                1.0e5 * patrolCapacity + compositionPrior;
            cs.push_back(std::move(c));
        }

        std::sort(cs.begin(), cs.end(), betterHeuristic);
        AgentSelectionResult res;
        res.evaluatedAssignments = cs.size();
        if (cs.empty()) {
            if (expired()) {
                res.success = true;
                res.timedOut = true;
                res.kinds = fallbackKinds(s, cfg, maxSupply);
                res.supplyCount = static_cast<std::int32_t>(std::count(
                    res.kinds.begin(), res.kinds.end(), AgentKind::Supply));
            }
            return res;
        }

        // 期限付き試走で全配置を候補にすると、最初の配置が
        // 全予算を使い切る。基本枠を固定し、各配置・交通シナリオへ
        // 残り時間を公平に分けて、台数と開始位置を実際に比較する。
        const std::size_t trialLimit =
            cfg.trialCandidateLimit > 0 && limited &&
                    cfg.extendTrialsToDeadline
                ? cs.size()
                : std::min(cfg.trialCandidateLimit, cs.size());
        std::vector<std::size_t> trials;
        std::size_t preferredSpread = cs.size();
        std::int32_t preferredSupply = cfg.minSupplyCount;
        bool supplyPressure = false;
        if (trialLimit > 0) {
            supplyPressure = s.fuelLimit <= maxDaySteps;
            // 短い締切で最低台数の候補だけを評価して終わらないよう、
            // 巡回車との比率がよい中心台数から近い順に比較する。
            const auto countOrder = supplyCountOrder(n, cfg, maxSupply);
            preferredSupply = countOrder.front();
            auto appendBest = [&](std::int32_t count) {
                if (trials.size() >= trialLimit) return;
                const auto found = std::find_if(cs.begin(), cs.end(),
                    [count](const Candidate& c) {
                        return c.supplyCount == count;
                    });
                if (found != cs.end())
                    trials.push_back(static_cast<std::size_t>(found - cs.begin()));
            };
            auto appendSpread = [&](std::int32_t count) {
                if (trials.size() >= trialLimit) return;
                std::size_t spread = cs.size();
                for (std::size_t i = 0; i < cs.size(); ++i) {
                    if (cs[i].supplyCount != count ||
                        std::find(trials.begin(), trials.end(), i) != trials.end())
                        continue;
                    if (spread == cs.size() ||
                        cs[i].supplySpread > cs[spread].supplySpread)
                        spread = i;
                }
                if (spread != cs.size()) {
                    trials.push_back(spread);
                    if (count == preferredSupply) preferredSpread = spread;
                }
            };

            // まず各台数の最良候補を比較し、実時間打切りで特定台数の
            // 位置違いだけに評価が偏らないようにする。
            for (const std::int32_t count : countOrder) {
                if (trials.size() >= trialLimit) break;
                appendBest(count);
            }
            // 各台数について、距離評価とは性質の異なる広域配置も試す。
            // 特定の台数の位置違いだけで候補枠を消費しない。
            for (const std::int32_t count : countOrder) {
                if (trials.size() >= trialLimit) break;
                appendSpread(count);
            }
            // 残りは「補給車1台で巡回車およそ2台」を中心に配分する。
            // 8台時は補給2台・3台の位置違いを交互に追加し、1台・4台の
            // 位置違いで候補枠を使い切らない。各台数の最良・広域配置は
            // 上で既に1件ずつ確保している。
            std::array<std::int32_t, 2> focusedCounts{
                preferredSupply,
                std::max(cfg.minSupplyCount, preferredSupply - 1),
            };
            bool added = true;
            while (added && trials.size() < trialLimit) {
                added = false;
                for (const std::int32_t count : focusedCounts) {
                    if (trials.size() >= trialLimit) break;
                    for (std::size_t i = 0; i < cs.size(); ++i) {
                        if (cs[i].supplyCount != count ||
                            std::find(trials.begin(), trials.end(), i) !=
                                trials.end()) {
                            continue;
                        }
                        trials.push_back(i);
                        added = true;
                        break;
                    }
                }
            }
            for (std::size_t i = 0;
                 i < cs.size() && trials.size() < trialLimit; ++i) {
                if (std::find(trials.begin(), trials.end(), i) == trials.end())
                    trials.push_back(i);
            }
        }

        // 候補枠が小さくても、時間切れ後の過少補給判定に使う中心台数の
        // 広域配置は取得しておく（試走件数には加えない）。
        if (cfg.trialCandidateLimit > 0 && preferredSpread == cs.size()) {
            for (std::size_t i = 0; i < cs.size(); ++i) {
                if (cs[i].supplyCount != preferredSupply) continue;
                if (!trials.empty() && i == trials.front()) continue;
                if (preferredSpread == cs.size() ||
                    cs[i].supplySpread > cs[preferredSpread].supplySpread) {
                    preferredSpread = i;
                }
            }
        }

        // 試走候補がある場合、完走前に締切となっても最少補給車へ偏る
        // 距離評価先頭ではなく、中心台数の最良候補を返す。
        const Candidate* best = trials.empty() ? &cs.front() : &cs[trials.front()];
        EvaluationBreakdown bestEval{};
        TrialScore bestWorst{}, bestTotal{};
        bool tried = false;
        BeamSearchConfig beam;
        beam.beamWidth = cfg.trialBeamWidth;
        beam.jointBeamWidth = cfg.trialJointBeamWidth;
        beam.weights = cfg.trialWeights;
        beam.threadCount = 1;
        beam.useStateDeadline = false;
        beam.iterativeDeepening = false;
        // 種別候補の比較では経路方式を増やさず、配置間へ同じ探索量を使う。
        beam.comparePeriodicReplanPlans = false;
        // Trials already simulate all days/scenarios; nested forecasting would
        // spend the assignment budget twice and bias partially completed trials.
        beam.useDayBeam = false;
        auto scenarios = cfg.trialScenarios.empty()
            ? makeTrialScenarios(s, cfg.trialScenarioCount)
            : cfg.trialScenarios;
        if (cfg.trialCandidateLimit > 0 && scenarios.empty())
            throw std::invalid_argument("at least one trial scenario is required");
        std::size_t completedRuns = 0;
        const std::size_t baseTrialCount =
            std::min(cfg.trialCandidateLimit, trials.size());
        const std::size_t plannedRuns = baseTrialCount * scenarios.size();
        for (std::size_t trialIndex = 0;
             trialIndex < trials.size(); ++trialIndex) {
            const std::size_t i = trials[trialIndex];
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
            TrialFirstDay firstDay;
            for (const auto& traffic : scenarios) {
                if (expired()) {
                    res.timedOut = true;
                    valid = false;
                    break;
                }
                auto trialDeadline = deadline;
                if (limited && trialIndex < baseTrialCount) {
                    const auto now = Clock::now();
                    const auto left = std::chrono::duration_cast<
                        std::chrono::milliseconds>(deadline - now).count();
                    const std::size_t remainingRuns =
                        std::max<std::size_t>(1, plannedRuns - completedRuns);
                    // 呼び出し間の処理用に5%を残す。早く完走した分は
                    // 次の配置へ自動的に繰り越される。
                    const auto sliceMs = std::max<std::int64_t>(
                        1, (left * 95 / 100) /
                               static_cast<std::int64_t>(remainingRuns));
                    trialDeadline = std::min(
                        deadline, now + std::chrono::milliseconds(sliceMs));
                }
                const auto trial = runTrial(
                    s, c.kinds, traffic, beam, cfg.trialDays, trialDeadline,
                    &firstDay);
                ++completedRuns;
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
                res.completedTrialScenarios = scenarios.size();
            }
        }

        res.success = true;
        res.timedOut = res.timedOut || expired();
        // 燃料制約下で比較が時間切れし、最低台数の構成へ偏った場合は、
        // 同じ系列到達性を保つ中心台数の広域配置を安全側として使う。
        // 全候補を時間内に比較できた結果は上書きしない。
        if (res.timedOut && supplyPressure && preferredSpread < cs.size() &&
            best->supplyCount < preferredSupply &&
            cs[preferredSpread].reachableBrandCount >=
                best->reachableBrandCount) {
            best = &cs[preferredSpread];
            tried = false;
            bestEval = {};
            bestWorst = {};
            bestTotal = {};
            res.completedTrialScenarios = 0;
        }
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
