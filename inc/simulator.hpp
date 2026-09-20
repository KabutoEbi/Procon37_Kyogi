#pragma once

#include "game_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace procon37 {

enum class SimulationError : std::uint8_t {
    None,
    StateInitializationFailed,
    AgentCountMismatch,
    MissingAction,
    ExtraAction,
    InvalidAction,
    InvalidPosition,
    ImpassableDestination,
    InsufficientFuel,
    ActionOverrunsDay,
    InternalStateError,
};

struct SimulationFailure {
    SimulationError error = SimulationError::None;
    AgentIndex agentIndex = 0;
    std::size_t actionIndex = 0;
    std::int32_t step = 0;
};

struct SimulationResult {
    bool valid = false;
    GameState state{};
    SimulationFailure failure{};

    // 自チームの各エージェントが各道路セルに滞在したステップ数の合計。
    std::vector<std::int32_t> roadStaySteps;
};

constexpr const char* simulationErrorMessage(SimulationError error) noexcept {
    switch (error) {
        case SimulationError::None:
            return "no error";
        case SimulationError::StateInitializationFailed:
            return "failed to initialize game state";
        case SimulationError::AgentCountMismatch:
            return "plan count does not match agent count";
        case SimulationError::MissingAction:
            return "plan ends before the day step limit";
        case SimulationError::ExtraAction:
            return "plan contains actions after the day step limit";
        case SimulationError::InvalidAction:
            return "action must be a direction from 0 to 5 or a negative wait";
        case SimulationError::InvalidPosition:
            return "agent position is outside the field";
        case SimulationError::ImpassableDestination:
            return "move destination is outside the field or is a pond";
        case SimulationError::InsufficientFuel:
            return "patrol agent does not have enough fuel";
        case SimulationError::ActionOverrunsDay:
            return "action cannot finish before the day step limit";
        case SimulationError::InternalStateError:
            return "simulator state is inconsistent";
    }
    return "unknown simulation error";
}

namespace detail {

inline SimulationResult simulationFailure(
    GameState st,
    std::vector<std::int32_t> roadSteps,
    SimulationError err,
    AgentIndex agent,
    std::size_t action,
    std::int32_t step) {
    SimulationResult res;
    res.state = st;
    res.roadStaySteps = std::move(roadSteps);
    res.failure = {err, agent, action, step};
    return res;
}

}  // namespace detail

// 現在のアクションフェーズで1台分の行動を予約する。
// ビームサーチでは、待機中でない全エージェントへ予約してから
// advanceSimulationStep() を呼ぶ。
inline SimulationFailure tryScheduleAction(
    GameState& st,
    const MatchSetting& s,
    const MatchState& ms,
    AgentIndex i,
    std::int32_t action,
    std::size_t actionIndex = 0) noexcept {
    auto fail = [&](SimulationError err) {
        return SimulationFailure{err, i, actionIndex, st.step};
    };

    if (st.finalized || i >= st.agentCount || st.pendingActions[i].active) {
        return fail(SimulationError::InternalStateError);
    }

    const Agent& a = st.agents[i];
    PendingAction next;
    next.active = true;
    next.rawAction = action;
    next.from = a.pos;

    if (isMoveAction(action)) {
        if (!s.field.contains(a.pos)) {
            return fail(SimulationError::InvalidPosition);
        }

        const auto dir = static_cast<Direction>(action);
        const CellPos to = s.field.neighbor(a.pos, dir);
        if (!s.field.contains(to) || !s.field.at(to).isPassable()) {
            return fail(SimulationError::ImpassableDestination);
        }

        next.move = true;
        next.destination = to;
        next.remainingSteps = moveStepCost(s, ms, a.pos);
        next.fuelCost = moveFuelCost(s, a.kind, a.pos);
        if (next.remainingSteps >= kImpassableCost ||
            next.fuelCost >= kImpassableCost) {
            return fail(SimulationError::InvalidPosition);
        }
        if (a.isPatrol() && a.fuel < next.fuelCost) {
            return fail(SimulationError::InsufficientFuel);
        }
    } else if (isWaitAction(action) &&
               action != std::numeric_limits<std::int32_t>::min()) {
        next.destination = a.pos;
        next.remainingSteps = -action;
    } else {
        return fail(SimulationError::InvalidAction);
    }

    if (next.remainingSteps <= 0 ||
        next.remainingSteps > st.remainingSteps()) {
        return fail(SimulationError::ActionOverrunsDay);
    }

    st.pendingActions[i] = next;
    return {};
}

// 反映フェーズを1ステップ進める。全エージェントに行動が予約済みで
// なければ失敗し、状態は変更しない。
inline SimulationFailure advanceSimulationStep(
    GameState& state,
    const MatchSetting& setting,
    std::vector<std::int32_t>* roadStaySteps = nullptr) noexcept {
    const std::int32_t nextStep = state.step + 1;
    if (state.finalized || nextStep > state.dayStepLimit) {
        return {
            SimulationError::ActionOverrunsDay,
            0,
            0,
            state.step,
        };
    }
    for (AgentIndex i = 0; i < state.agentCount; ++i) {
        if (!state.pendingActions[i].active ||
            state.pendingActions[i].remainingSteps <= 0) {
            return {SimulationError::MissingAction, i, 0, state.step};
        }
    }
    if (roadStaySteps != nullptr &&
        roadStaySteps->size() != setting.field.cellCount()) {
        return {
            SimulationError::InternalStateError,
            0,
            0,
            state.step,
        };
    }

    if (!state.advanceTo(nextStep)) {
        return {
            SimulationError::InternalStateError,
            0,
            0,
            state.step,
        };
    }

    // 燃料消費と移動は全車分を確定してから取得処理へ進む。
    for (AgentIndex i = 0; i < state.agentCount; ++i) {
        PendingAction& pending = state.pendingActions[i];
        --pending.remainingSteps;
        if (pending.remainingSteps != 0) {
            continue;
        }

        Agent& agent = state.agents[i];
        if (pending.isMove()) {
            if (agent.pos != pending.from ||
                (agent.isPatrol() && agent.fuel < pending.fuelCost)) {
                return {
                    SimulationError::InternalStateError,
                    i,
                    0,
                    state.step,
                };
            }
            if (agent.isPatrol()) {
                agent.fuel -= pending.fuelCost;
            }
            agent.pos = pending.destination;
        }
        pending = PendingAction{};
    }

    for (AgentIndex i = 0; i < state.agentCount; ++i) {
        const CellPos pos = state.agents[i].pos;
        if (!setting.field.contains(pos)) {
            return {
                SimulationError::InternalStateError,
                i,
                0,
                state.step,
            };
        }
        const Cell& cell = setting.field.at(pos);
        if (cell.hasSpot()) {
            state.tryAcquire(
                i,
                static_cast<std::size_t>(cell.spotIndex),
                setting.field);
        }
    }

    state.refuelCoLocated(setting.fuelLimit);

    if (roadStaySteps != nullptr) {
        for (AgentIndex i = 0; i < state.agentCount; ++i) {
            const CellPos pos = state.agents[i].pos;
            if (setting.field.at(pos).type == CellType::Road) {
                ++(*roadStaySteps)[static_cast<std::size_t>(pos)];
            }
        }
    }
    return {};
}

// 完成済みの1日分の計画を、公式のフェーズ順序で実行する。
inline SimulationResult simulateDay(
    const MatchSetting& s,
    const MatchState& ms,
    const DailyPlan& plan,
    ScoreState score = {}) {
    GameState st;
    std::vector<std::int32_t> roadSteps(s.field.cellCount(), 0);

    if (!st.initialize(s, ms, score)) {
        return detail::simulationFailure(
            st,
            std::move(roadSteps),
            SimulationError::StateInitializationFailed,
            0,
            0,
            0);
    }
    if (plan.size() != st.agentCount) {
        return detail::simulationFailure(
            st,
            std::move(roadSteps),
            SimulationError::AgentCountMismatch,
            0,
            0,
            0);
    }

    std::array<std::size_t, kMaxAgents> cursors{};

    auto fail = [&](SimulationError err,
                    AgentIndex agent,
                    std::size_t action,
                    std::int32_t step) {
        return detail::simulationFailure(
            st, roadSteps, err, agent, action, step);
    };
    auto failWith = [&](const SimulationFailure& f) {
        return fail(f.error, f.agentIndex, f.actionIndex, f.step);
    };

    // step 0: アクションフェーズのみ。
    for (AgentIndex i = 0; i < st.agentCount; ++i) {
        if (plan[i].empty()) {
            return fail(SimulationError::MissingAction, i, 0, 0);
        }
        const auto err = tryScheduleAction(st, s, ms, i, plan[i][0], 0);
        if (err.error != SimulationError::None) {
            return failWith(err);
        }
        ++cursors[i];
    }

    for (std::int32_t step = 1; step <= st.dayStepLimit; ++step) {
        const auto err = advanceSimulationStep(st, s, &roadSteps);
        if (err.error != SimulationError::None) {
            return failWith(err);
        }

        // 最終ステップにはアクションフェーズがない。
        if (step == st.dayStepLimit) {
            continue;
        }
        for (AgentIndex i = 0; i < st.agentCount; ++i) {
            if (!st.pendingActions[i].active) {
                if (cursors[i] >= plan[i].size()) {
                    return fail(
                        SimulationError::MissingAction,
                        i,
                        cursors[i],
                        step);
                }
                const auto err = tryScheduleAction(
                    st, s, ms, i, plan[i][cursors[i]], cursors[i]);
                if (err.error != SimulationError::None) {
                    return failWith(err);
                }
                ++cursors[i];
            }
        }
    }

    for (AgentIndex i = 0; i < st.agentCount; ++i) {
        if (st.pendingActions[i].active) {
            return fail(
                SimulationError::InternalStateError,
                i,
                cursors[i],
                st.dayStepLimit);
        }
        if (cursors[i] != plan[i].size()) {
            return fail(
                SimulationError::ExtraAction,
                i,
                cursors[i],
                st.dayStepLimit);
        }
    }
    if (!st.finishDay()) {
        return fail(
            SimulationError::InternalStateError,
            0,
            0,
            st.dayStepLimit);
    }

    SimulationResult res;
    res.valid = true;
    res.state = st;
    res.roadStaySteps = std::move(roadSteps);
    return res;
}

}  // namespace procon37
