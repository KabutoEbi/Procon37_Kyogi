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
    GameState state,
    std::vector<std::int32_t> roadStaySteps,
    SimulationError error,
    AgentIndex agentIndex,
    std::size_t actionIndex,
    std::int32_t step) {
    SimulationResult result;
    result.state = state;
    result.roadStaySteps = std::move(roadStaySteps);
    result.failure = {error, agentIndex, actionIndex, step};
    return result;
}

}  // namespace detail

// 現在のアクションフェーズで1台分の行動を予約する。
// ビームサーチでは、待機中でない全エージェントへ予約してから
// advanceSimulationStep() を呼ぶ。
inline SimulationFailure tryScheduleAction(
    GameState& state,
    const MatchSetting& setting,
    const MatchState& matchState,
    AgentIndex agentIndex,
    std::int32_t rawAction,
    std::size_t actionIndex = 0) noexcept {
    auto failure = [&](SimulationError error) {
        return SimulationFailure{
            error,
            agentIndex,
            actionIndex,
            state.step,
        };
    };

    if (state.finalized || agentIndex >= state.agentCount ||
        state.pendingActions[agentIndex].active) {
        return failure(SimulationError::InternalStateError);
    }

    const Agent& agent = state.agents[agentIndex];
    PendingAction pending;
    pending.active = true;
    pending.rawAction = rawAction;
    pending.from = agent.pos;

    if (isMoveAction(rawAction)) {
        if (!setting.field.contains(agent.pos)) {
            return failure(SimulationError::InvalidPosition);
        }

        const Direction direction = static_cast<Direction>(rawAction);
        const CellPos destination =
            setting.field.neighbor(agent.pos, direction);
        if (!setting.field.contains(destination) ||
            !setting.field.at(destination).isPassable()) {
            return failure(SimulationError::ImpassableDestination);
        }

        pending.move = true;
        pending.destination = destination;
        pending.remainingSteps =
            moveStepCost(setting, matchState, agent.pos);
        pending.fuelCost =
            moveFuelCost(setting, agent.kind, agent.pos);
        if (pending.remainingSteps >= kImpassableCost ||
            pending.fuelCost >= kImpassableCost) {
            return failure(SimulationError::InvalidPosition);
        }
        if (agent.isPatrol() && agent.fuel < pending.fuelCost) {
            return failure(SimulationError::InsufficientFuel);
        }
    } else if (isWaitAction(rawAction) &&
               rawAction != std::numeric_limits<std::int32_t>::min()) {
        pending.move = false;
        pending.destination = agent.pos;
        pending.remainingSteps = -rawAction;
    } else {
        return failure(SimulationError::InvalidAction);
    }

    if (pending.remainingSteps <= 0 ||
        pending.remainingSteps > state.dayStepLimit - state.step) {
        return failure(SimulationError::ActionOverrunsDay);
    }

    state.pendingActions[agentIndex] = pending;
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
    const MatchSetting& setting,
    const MatchState& matchState,
    const DailyPlan& plan,
    ScoreState previousScore = {}) {
    GameState state;
    std::vector<std::int32_t> roadStaySteps(
        setting.field.cellCount(), 0);

    if (!state.initialize(setting, matchState, previousScore)) {
        return detail::simulationFailure(
            state,
            std::move(roadStaySteps),
            SimulationError::StateInitializationFailed,
            0,
            0,
            0);
    }
    if (plan.size() != state.agentCount) {
        return detail::simulationFailure(
            state,
            std::move(roadStaySteps),
            SimulationError::AgentCountMismatch,
            0,
            0,
            0);
    }

    std::array<std::size_t, kMaxAgents> cursors{};

    auto fail = [&](SimulationError error,
                    AgentIndex agentIndex,
                    std::size_t actionIndex,
                    std::int32_t step) {
        return detail::simulationFailure(
            state,
            roadStaySteps,
            error,
            agentIndex,
            actionIndex,
            step);
    };

    // step 0: アクションフェーズのみ。
    for (AgentIndex i = 0; i < state.agentCount; ++i) {
        if (plan[i].empty()) {
            return fail(SimulationError::MissingAction, i, 0, 0);
        }
        const SimulationFailure failure = tryScheduleAction(
            state, setting, matchState, i, plan[i][0], 0);
        if (failure.error != SimulationError::None) {
            return fail(
                failure.error,
                failure.agentIndex,
                failure.actionIndex,
                failure.step);
        }
        ++cursors[i];
    }

    for (std::int32_t step = 1; step <= state.dayStepLimit; ++step) {
        const SimulationFailure advanceFailure =
            advanceSimulationStep(state, setting, &roadStaySteps);
        if (advanceFailure.error != SimulationError::None) {
            return fail(
                advanceFailure.error,
                advanceFailure.agentIndex,
                advanceFailure.actionIndex,
                advanceFailure.step);
        }

        // 最終ステップにはアクションフェーズがない。
        if (step == state.dayStepLimit) {
            continue;
        }
        for (AgentIndex i = 0; i < state.agentCount; ++i) {
            if (!state.pendingActions[i].active) {
                if (cursors[i] >= plan[i].size()) {
                    return fail(
                        SimulationError::MissingAction,
                        i,
                        cursors[i],
                        step);
                }
                const SimulationFailure failure = tryScheduleAction(
                    state,
                    setting,
                    matchState,
                    i,
                    plan[i][cursors[i]],
                    cursors[i]);
                if (failure.error != SimulationError::None) {
                    return fail(
                        failure.error,
                        failure.agentIndex,
                        failure.actionIndex,
                        failure.step);
                }
                ++cursors[i];
            }
        }
    }

    for (AgentIndex i = 0; i < state.agentCount; ++i) {
        if (state.pendingActions[i].active) {
            return fail(
                SimulationError::InternalStateError,
                i,
                cursors[i],
                state.dayStepLimit);
        }
        if (cursors[i] != plan[i].size()) {
            return fail(
                SimulationError::ExtraAction,
                i,
                cursors[i],
                state.dayStepLimit);
        }
    }
    if (!state.finishDay()) {
        return fail(
            SimulationError::InternalStateError,
            0,
            0,
            state.dayStepLimit);
    }

    SimulationResult result;
    result.valid = true;
    result.state = state;
    result.roadStaySteps = std::move(roadStaySteps);
    return result;
}

}  // namespace procon37
