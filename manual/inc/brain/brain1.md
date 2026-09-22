# `brain/brain1.hpp` の使い方

対象: [`inc/brain/brain1.hpp`](../../../inc/brain/brain1.hpp)

全エージェントの1日分の行動計画をビームサーチで生成するソルバー本体である。

## 基本的な呼び出し

```cpp
procon37::BeamSearchConfig config;
config.beamWidth = 128;
config.jointBeamWidth = 512;
config.timeLimitMs = 2500;

procon37::ScoreState previousScore;
auto result = procon37::brain1(
    setting, matchState, previousScore, config);

if (result.success) {
    const procon37::DailyPlan& plan = result.plan;
}
```

## 主要設定

- `beamWidth`: 各展開で残す状態数
- `jointBeamWidth`: 同時に空いた複数台へ行動を割り当てる途中幅
- `timeLimitMs`: 相対時間制限。0なら指定なし
- `useStateDeadline`: `MatchState::endsAt`も締切に使う
- `safetyMarginMs`: 締切前に残す余裕
- `threadCount`: 0で自動、1で並列無効
- `maxActionsPerAgent`: 各台で残す合法行動数。0で全候補
- `useMacroActions`: スポットや合流点への経路をまとめて生成
- `dominancePruning`: 燃料優越状態を削除
- `diversityFraction`: 異なる系列・位置へ確保するビーム割合
- `weights`: `EvaluationWeights`

## 結果

`BeamSearchResult`には計画、最終状態、評価内訳、展開数、評価回数、マクロ統計、経過時間、停止理由が入る。`success == false`の場合は`plan`を送信せず、`application.hpp`の`solveDay()`などで待機計画へフォールバックする。

結果は内部でも検証されるが、外部から組み替えた計画は`validateDailyPlan()`で再検査する。

補給込み経路は1回の合流で目的地まで届かない場合、到達可能な合流点でマクロを区切る。補給反映後に次の合流点を再計算するため、同じ日・同じ目的地までに複数回の補給が必要な経路も扱える。
