# `weight_tuner.hpp` の使い方

対象: [`inc/weight_tuner.hpp`](../../inc/weight_tuner.hpp)

学習問題と独立した検証問題を使って、探索誘導用の`EvaluationWeights`をオフライン調整する。

```cpp
std::vector<benzen::WeightTuningCase> training = /* ... */;
std::vector<benzen::WeightTuningCase> validation = /* ... */;

benzen::WeightTuningConfig config;
config.candidates = 24;
config.scale = 1.5;
config.seed = 1;

auto tuned = benzen::tuneWeights(
    training, validation, {}, config);
benzen::printTunedWeights(std::cout, tuned.weights);
```

各`WeightTuningCase::id`は学習・検証集合を通して一意にする。同じ問題IDの重複は例外になる。

調整は公式得点の主要重みを固定し、距離、燃料、補給、翌日価値など`kTunableWeights`の項目だけを乗法的に探索する。候補は学習集合で選び、検証得点が初期値未満なら初期重みに戻す。

`evaluateWeights()`は複数ケースの`TrialScore`合計を返す。再現可能な比較のため内部で1スレッドに固定される。本番実行中ではなく、事前調整ツールとして使う。
