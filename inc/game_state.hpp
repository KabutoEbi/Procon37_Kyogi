#pragma once

#include "game.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace benzen {

constexpr std::uint32_t countBits(BrandMask bits) noexcept {
    std::uint32_t count = 0;
    while (bits != 0) {
        bits &= bits - 1;
        ++count;
    }
    return count;
}

constexpr SpotMask spotBit(std::size_t spotIndex) noexcept {
    return spotIndex < 64 ? SpotMask{1} << spotIndex : SpotMask{0};
}

constexpr BrandMask brandBit(std::size_t brandIndex) noexcept {
    return brandIndex < 64 ? BrandMask{1} << brandIndex : BrandMask{0};
}

// 日をまたいで引き継ぐ、自チームの確定済み得点情報。
struct ScoreState {
    BrandMask matchBrands = 0;
    std::int32_t totalUdon = 0;
    std::int32_t sumOfDailyBrandCounts = 0;
};

// 日をまたぐ累積得点を再起動後に復元するためのチェックポイント。
struct ScoreCheckpoint {
    std::int32_t nextDay = 0;
    ScoreState score{};
};

// ステップ進行中の1エージェントの予約行動。
// remainingSteps が0になった反映フェーズで、移動と燃料消費を確定する。
struct PendingAction {
    bool active = false;
    bool move = false;
    std::int32_t rawAction = 0;
    std::int32_t remainingSteps = 0;
    CellPos from = kInvalidCell;
    CellPos destination = kInvalidCell;
    std::int32_t fuelCost = 0;

    constexpr bool isMove() const noexcept {
        return active && move;
    }

    constexpr bool isWait() const noexcept {
        return active && !move;
    }
};

// 1日分の探索・シミュレーションで変化する状態。
// 固定長配列のみなので、ビームサーチのノードとして安価にコピーできる。
class GameState {
public:
    std::int32_t day = 0;
    std::int32_t step = 0;
    std::int32_t dayStepLimit = 0;

    std::size_t agentCount = 0;
    std::size_t spotCount = 0;
    std::array<Agent, kMaxAgents> agents{};
    std::array<PendingAction, kMaxAgents> pendingActions{};

    // Field::spots と同じ添字。
    std::array<std::int32_t, kMaxSpots> remainingStocks{};

    // 各巡回車が当日に取得済みのスポット。補給車の要素は常に0。
    std::array<SpotMask, kMaxAgents> acquiredSpots{};

    BrandMask dayBrands = 0;
    std::int32_t dayUdon = 0;
    ScoreState score{};

    // finishDay() の二重反映を防ぐ。
    bool finalized = false;

    // API状態から当日開始時の状態を作る。失敗時は自身を変更しない。
    bool initialize(
        const MatchSetting& s,
        const MatchState& ms,
        ScoreState prev = {}) noexcept {
        if (!s.hasConsistentDaySettings() || ms.day < 0 ||
            static_cast<std::size_t>(ms.day) >= s.dayCount() ||
            ms.agents.size() != s.agentCount() ||
            ms.agents.size() > kMaxAgents ||
            s.field.spots.size() > kMaxSpots ||
            s.field.brands.size() > kMaxBrands) {
            return false;
        }

        GameState next;
        next.day = ms.day;
        next.dayStepLimit = s.daySteps[static_cast<std::size_t>(ms.day)];
        if (next.dayStepLimit < 0) {
            return false;
        }

        next.agentCount = ms.agents.size();
        next.spotCount = s.field.spots.size();
        next.score = prev;

        for (std::size_t i = 0; i < next.agentCount; ++i) {
            next.agents[i] = ms.agents[i];
            next.agents[i].index = i;
            if (!s.field.contains(next.agents[i].pos) ||
                next.agents[i].fuel < 0) {
                return false;
            }
        }
        for (std::size_t i = 0; i < next.spotCount; ++i) {
            const Spot& spot = s.field.spots[i];
            if (spot.maxStock <= 0 || spot.brandIndex < 0 ||
                static_cast<std::size_t>(spot.brandIndex) >=
                    s.field.brands.size()) {
                return false;
            }
            next.remainingStocks[i] = spot.maxStock;
        }

        *this = next;
        return true;
    }

    constexpr std::int32_t remainingSteps() const noexcept {
        return dayStepLimit - step;
    }

    bool advanceTo(std::int32_t nextStep) noexcept {
        if (finalized || nextStep < step || nextStep > dayStepLimit) {
            return false;
        }
        step = nextStep;
        return true;
    }

    bool hasAcquired(AgentIndex agentIndex, std::size_t spotIndex) const noexcept {
        if (agentIndex >= agentCount || spotIndex >= spotCount) {
            return false;
        }
        return (acquiredSpots[agentIndex] & spotBit(spotIndex)) != 0;
    }

    // 取得フェーズでエージェント添字の昇順に呼ぶ。
    // 取得条件を満たした場合だけ状態を更新する。
    bool tryAcquire(
        AgentIndex agentIndex,
        std::size_t spotIndex,
        const Field& field) noexcept {
        if (finalized || agentIndex >= agentCount || spotIndex >= spotCount ||
            spotIndex >= field.spots.size()) {
            return false;
        }

        Agent& agent = agents[agentIndex];
        const Spot& spot = field.spots[spotIndex];
        if (!agent.isPatrol() || agent.pos != spot.pos ||
            remainingStocks[spotIndex] <= 0 ||
            hasAcquired(agentIndex, spotIndex) || spot.brandIndex < 0 ||
            static_cast<std::size_t>(spot.brandIndex) >= field.brands.size()) {
            return false;
        }

        --remainingStocks[spotIndex];
        acquiredSpots[agentIndex] |= spotBit(spotIndex);
        const BrandMask bit =
            brandBit(static_cast<std::size_t>(spot.brandIndex));
        dayBrands |= bit;
        score.matchBrands |= bit;
        ++dayUdon;
        ++score.totalUdon;
        return true;
    }

    // 補給フェーズで呼ぶ。同じセルに補給車がいれば巡回車を満タンにする。
    void refuelCoLocated(std::int32_t fuelLimit) noexcept {
        if (finalized || fuelLimit < 0) {
            return;
        }
        for (std::size_t p = 0; p < agentCount; ++p) {
            if (!agents[p].isPatrol()) {
                continue;
            }
            for (std::size_t q = 0; q < agentCount; ++q) {
                if (agents[q].isSupply() && agents[q].pos == agents[p].pos) {
                    agents[p].fuel = fuelLimit;
                    break;
                }
            }
        }
    }

    // 当日の系列種類数を累積値へ確定する。一度だけ成功する。
    bool finishDay() noexcept {
        if (finalized || step != dayStepLimit) {
            return false;
        }
        score.sumOfDailyBrandCounts +=
            static_cast<std::int32_t>(countBits(dayBrands));
        finalized = true;
        return true;
    }
};

}  // namespace benzen
