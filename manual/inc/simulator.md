# `simulator.hpp` の使い方

対象: [`inc/simulator.hpp`](../../inc/simulator.hpp)

1日分の計画を公式のフェーズ順で実行し、移動、取得、補給、得点を更新する。公式Q&A Q7に従い、step 0は行動予約だけを行い、最初の取得はstep 1の移動反映後の位置で判定する。移動に2step以上かかる場合はstep 1でも元のセルにいるため開始スポットを取得するが、順調な道路から1stepで離れる場合は取得しない。

## 計画を一括実行

```cpp
auto result = benzen::simulateDay(
    setting, matchState, plan, previousScore);

if (!result.valid) {
    std::cerr << benzen::simulationErrorMessage(result.failure.error)
              << " agent=" << result.failure.agentIndex
              << " action=" << result.failure.actionIndex << '\n';
} else {
    const auto& end = result.state;
}
```

`SimulationResult::roadStaySteps`には、自チームが各道路セルに滞在した合計ステップ数が入る。

## 1ステップずつ実行

```cpp
benzen::GameState game;
game.initialize(setting, matchState, previousScore);

for (std::size_t i = 0; i < game.agentCount; ++i) {
    benzen::tryScheduleAction(game, setting, matchState, i, -1);
}
auto failure = benzen::advanceSimulationStep(game, setting);
```

各ステップの前に、待機中でない全エージェントへ行動を予約する必要がある。通常は実装ミスを避けるため`simulateDay()`を使う。

最終ステップを過不足なく埋める計画が必要で、余分・不足・池への移動・燃料不足などは`SimulationError`で返る。道路状態を反映した移動時間が残りステップを超える場合は`ActionOverrunsDay`となり、探索ではその移動を候補から除外して待機などの完了可能な行動を選ぶ。
