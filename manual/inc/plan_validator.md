# `plan_validator.hpp` の使い方

対象: [`inc/plan_validator.hpp`](../../inc/plan_validator.hpp)

完成した`DailyPlan`がルール上有効かを検証する薄い入口である。

```cpp
auto validation = procon37::validateDailyPlan(
    setting, matchState, plan, previousScore);

if (!validation.valid) {
    const auto& failure = validation.failure;
    std::cerr << procon37::simulationErrorMessage(failure.error) << '\n';
}
```

内部で`simulateDay()`を使うため、ステップ数だけでなく、移動先、燃料、補給のタイミング、取得処理まで本番と同じ順序で検査する。

検証後の最終状態や道路滞在時間も必要なら、直接`simulateDay()`を呼ぶ。
