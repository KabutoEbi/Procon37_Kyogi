# `agent.hpp` の使い方

対象: [`inc/agent.hpp`](../../inc/agent.hpp)

エージェント、他チーム、および回答用の行動列を定義する。

## エージェント

```cpp
procon37::Agent agent{
    0, procon37::AgentKind::Patrol, 12, 30
};

if (agent.isPatrol()) {
    // fuelを消費して移動する巡回車
}
```

`Agent::index`は`MatchState::agents`内の不変な添字として使う。`TeamState`は他チームIDとそのエージェント列を保持する。

## 行動計画

```cpp
procon37::AgentPlan one = {2, -1, -1, -3, 5};
one = procon37::compressWaitActions(one);
// {2, -5, 5}

procon37::DailyPlan all = {
    one,
    {-10},
};
```

`AgentPlan`は1台分、`DailyPlan`は全台分の計画である。`appendPlanAction()`を使うと、末尾の連続待機を追加時にまとめられる。移動値は0～5、待機は`-N`である。
