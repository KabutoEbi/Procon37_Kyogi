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

`EvaluationWeights`の既定値は、公式の辞書順である試合系列数、日別系列数、玉数を強く優先する。距離、補給、燃料危険、翌日価値などは探索誘導用である。

`EvaluationContext`の距離配列はコンストラクタで到達不能値に初期化される。`EvaluationBreakdown`には各項目と合計が残るため、重み調整やコンソール表示に使える。内訳から合計だけを再計算する場合は`evaluationTotal()`を使う。
