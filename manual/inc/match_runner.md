# `match_runner.hpp` の使い方

対象: [`inc/match_runner.hpp`](../../inc/match_runner.hpp)

同じエージェント種別で全日を順番に探索・シミュレーションし、位置、燃料、得点を翌日へ引き継ぐ。

```cpp
benzen::MatchRunnerConfig config;
config.beam.beamWidth = 128;
config.fallbackToWait = true;

auto result = benzen::runMatch(setting, kinds, config);
if (result.success) {
    auto finalScore = result.score;
    for (const auto& day : result.days) {
        // day.plan、day.endState、day.evaluationを確認
    }
}
```

`trafficByDay`を空にすると全道路を毎日`Smooth`として実行する。指定する場合は`日数 × field.cellCount()`の配列が必要で、各日の探索にはその日の道路状態だけが渡される。

各日は公開入口`brain1()`を呼ぶため、設定条件を満たす大規模盤面の最終日では通常経路と周期再計画経路の比較も適用される。比較条件は[`brain1.hpp`の説明](brain/brain1.md)を参照する。

探索失敗時、`fallbackToWait=true`なら全車待機計画で続行する。無効なら`failedDay`と`failure`を設定して終了する。

`MatchDayResult`には開始状態、計画、終了状態、評価、道路滞在時間、展開数、探索時間`elapsedMs`、割当キャッシュ命中数`assignmentCacheHits`、割当計算数`assignmentComputations`、停止理由が保存される。

`beam.profilePlanning`を有効にした場合は、`assignmentTimeNs`・`flowTimeNs`・`rendezvousTimeNs`も日別に保存する。
