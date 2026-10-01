#pragma once

#include "game.hpp"
#include "simulator.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace benzen {

// 未実行経路のsuffixを共有する。等しい残り方向列は同じIDになる。
class ActionRoutes {
public:
    using Id = std::uint32_t;
    static constexpr Id none = std::numeric_limits<Id>::max();
    struct Entry { std::int32_t action; Id next; };

    Id intern(const AgentPlan& actions) {
        Id tail = none;
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            const std::uint64_t key =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(*it)) << 32) |
                tail;
            const auto found = ids_.find(key);
            if (found != ids_.end()) {
                tail = found->second;
            } else {
                const Id id = static_cast<Id>(entries_.size());
                entries_.push_back({*it, tail});
                ids_.emplace(key, id);
                tail = id;
            }
        }
        return tail;
    }

    Entry operator[](Id id) const { return entries_[id]; }

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::uint64_t, Id> ids_;
};

// planはstateまでの予約済み行動を含む。中断時の部分planは呼出し側で破棄する。
// 補給や在庫の変化は通常のシミュレータと同じフェーズで反映する。
template <class Stop>
inline bool appendRemainingRoutes(
    GameState state, std::array<ActionRoutes::Id, kMaxAgents> tails,
    const ActionRoutes& routes, const MatchSetting& s, const MatchState& ms,
    DailyPlan& plan, Stop&& stopped) {
    if (state.agentCount > kMaxAgents || plan.size() != state.agentCount) return false;
    while (state.remainingSteps() > 0) {
        if (stopped()) return false;
        for (AgentIndex a = 0; a < state.agentCount; ++a) {
            if (state.pendingActions[a].active) continue;
            int action = -state.remainingSteps();
            if (tails[a] != ActionRoutes::none) {
                const auto next = routes[tails[a]];
                action = next.action;
                tails[a] = next.next;
                if (tryScheduleAction(state, s, ms, a, action).error !=
                    SimulationError::None) {
                    tails[a] = ActionRoutes::none;
                    action = -state.remainingSteps();
                } else {
                    appendPlanAction(plan[a], action);
                    continue;
                }
            }
            if (tryScheduleAction(state, s, ms, a, action).error !=
                SimulationError::None) return false;
            appendPlanAction(plan[a], action);
        }
        if (advanceSimulationStep(state, s).error != SimulationError::None)
            return false;
    }
    return true;
}

}  // namespace benzen
