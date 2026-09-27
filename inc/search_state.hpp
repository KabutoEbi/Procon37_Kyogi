#pragma once

#include "game_state_hash.hpp"

namespace benzen {

// 完全比較APIを変えず、探索だけで将来の挙動が等価な状態を統合する。
inline GameState canonicalSearchState(GameState state, bool ignoreFuel = false) {
    for (AgentIndex a = 0; a < state.agentCount; ++a) {
        auto& pending = state.pendingActions[a];
        if (!pending.active) {
            pending = PendingAction{};
        } else {
            pending.rawAction = pending.move ? 0 : -pending.remainingSteps;
            if (!pending.move) {
                pending.from = state.agents[a].pos;
                pending.destination = state.agents[a].pos;
                pending.fuelCost = 0;
            }
        }
        if (ignoreFuel || state.agents[a].isSupply()) state.agents[a].fuel = 0;
    }
    return state;
}

inline bool fuelDominates(const GameState& lhs, const GameState& rhs) noexcept {
    for (AgentIndex a = 0; a < lhs.agentCount; ++a)
        if (lhs.agents[a].isPatrol() && lhs.agents[a].fuel < rhs.agents[a].fuel)
            return false;
    return true;
}

// 在庫・取得済みスポット・系列・得点を同一に限定する安全な優越判定。
inline bool searchStateDominates(const GameState& lhs, const GameState& rhs) {
    return canonicalSearchState(lhs, true) == canonicalSearchState(rhs, true) &&
           fuelDominates(lhs, rhs);
}

}  // namespace benzen
