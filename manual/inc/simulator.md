# `simulator.hpp` の使い方

対象: [`inc/simulator.hpp`](../../inc/simulator.hpp)

1日分の計画を公式のフェーズ順で実行し、移動、取得、補給、得点を更新する。

## 計画を一括実行

```cpp
auto result = procon37::simulateDay(
    setting, matchState, plan, previousScore);

if (!result.valid) {
    std::cerr << procon37::simulationErrorMessage(result.failure.error)
              << " agent=" << result.failure.agentIndex
              << " action=" << result.failure.actionIndex << '\n';
} else {
    const auto& end = result.state;
}
```

`SimulationResult::roadStaySteps`には、自チームが各道路セルに滞在した合計ステップ数が入る。

## 1ステップずつ実行

```cpp
procon37::GameState game;
game.initialize(setting, matchState, previousScore);

for (std::size_t i = 0; i < game.agentCount; ++i) {
    procon37::tryScheduleAction(game, setting, matchState, i, -1);
}
auto failure = procon37::advanceSimulationStep(game, setting);
```

各ステップの前に、待機中でない全エージェントへ行動を予約する必要がある。通常は実装ミスを避けるため`simulateDay()`を使う。

最終ステップを過不足なく埋める計画が必要で、余分・不足・池への移動・燃料不足などは`SimulationError`で返る。
