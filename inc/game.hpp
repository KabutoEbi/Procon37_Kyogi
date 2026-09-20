#pragma once

#include "agent.hpp"
#include "field.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace procon37 {

// GET /setting の内容。JSON の map は field に対応する。
struct MatchSetting {
    std::int64_t startsAt = 0;
    std::vector<std::int32_t> daySeconds;
    std::vector<std::int32_t> daySteps;
    Field field;
    std::vector<CellPos> agentStarts;
    std::int32_t fuelLimit = 0;
    std::int32_t playerCount = 0;
    std::int32_t busyThreshold = 0;
    std::int32_t jammedThreshold = 0;

    std::size_t dayCount() const noexcept {
        return daySteps.size();
    }

    std::size_t agentCount() const noexcept {
        return agentStarts.size();
    }

    bool hasConsistentDaySettings() const noexcept {
        return daySteps.size() == daySeconds.size();
    }
};

// GET / の内容を探索向けに保持する。
struct MatchState {
    std::int64_t endsAt = 0;
    std::int32_t day = 0;
    // GET / の agents。自チームIDはAPIから与えられない。
    std::vector<Agent> agents;
    std::vector<TeamState> others;

    // field.cells と同じ添字で引く。非道路セルの値は参照しない。
    std::vector<TrafficStatus> trafficByPos;

    TrafficStatus trafficAt(CellPos pos) const noexcept {
        if (pos < 0 || static_cast<std::size_t>(pos) >= trafficByPos.size()) {
            return TrafficStatus::Smooth;
        }
        return trafficByPos[static_cast<std::size_t>(pos)];
    }
};

// APIの traffics 配列を、ダイクストラでO(1)参照できる配列へ変換する。
inline std::vector<TrafficStatus> makeTrafficTable(
    const Field& field,
    const std::vector<TrafficInfo>& traffics) {
    std::vector<TrafficStatus> result(
        field.cellCount(), TrafficStatus::Smooth);
    for (const TrafficInfo& traffic : traffics) {
        if (field.contains(traffic.pos) &&
            field.at(traffic.pos).type == CellType::Road) {
            result[static_cast<std::size_t>(traffic.pos)] = traffic.status;
        }
    }
    return result;
}

// from から隣へ移動するコスト。from が池なら kImpassableCost。
inline std::int32_t moveStepCost(
    const MatchSetting& setting,
    const MatchState& state,
    CellPos from) noexcept {
    if (!setting.field.contains(from)) {
        return kImpassableCost;
    }
    const Cell& cell = setting.field.at(from);
    return cell.moveSteps(state.trafficAt(from));
}

inline std::int32_t moveFuelCost(
    const MatchSetting& setting,
    AgentKind kind,
    CellPos from) noexcept {
    if (!setting.field.contains(from)) {
        return kImpassableCost;
    }
    return kind == AgentKind::Supply
               ? 0
               : setting.field.at(from).patrolFuelCost();
}

}  // namespace procon37
