# `evaluation.hpp` の使い方

対象: [`inc/evaluation.hpp`](../../inc/evaluation.hpp)

ビームサーチ状態を評価する重み、補助情報、評価内訳を定義する。

```cpp
benzen::EvaluationWeights weights;
weights.remainingFuel = 2.0;
weights.fuelRisk = -2000.0;

benzen::EvaluationContext context;
context.scoreAtDayStart = previousScore;
context.nearestValuableSpotSteps[0] = 5;

auto result = benzen::evaluateGameState(state, context, weights);
double score = result.total;
```

`EvaluationWeights`の既定値は、公式の辞書順である試合系列数、日別系列数、玉数を強く優先する。さらに盤面の全系列を揃えた状態には、試合系列で`1.0e15`、当日系列で`1.0e11`の完達ボーナスを加える。通常の1系列分はそれぞれ`1.0e12`、`1.0e8`なので、最大系列へ到達した候補をビーム内に強く残しつつ、試合系列が日別系列より上位という順序を維持する。`EvaluationContext.availableBrandCount`が0の場合は完達判定を行わない。距離、補給、燃料危険、翌日価値などは探索誘導用である。

`EvaluationContext`の距離配列はコンストラクタで到達不能値に初期化される。`EvaluationBreakdown`には`matchBrandCompleted`、`dailyBrandCompleted`を含む各項目と合計が残るため、重み調整やコンソール表示に使える。内訳から合計だけを再計算する場合は`evaluationTotal()`を使う。
