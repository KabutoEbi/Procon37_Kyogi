# `tune_weights.cpp` の使い方

対象: [`tests/tune_weights.cpp`](../../tests/tune_weights.cpp)

固定seedのランダム問題を学習用と検証用に分け、`EvaluationWeights`をオフライン調整する実行ツールである。

## 実行

```sh
c++ -std=c++17 -O2 -pthread -Iinc tests/tune_weights.cpp -o /tmp/tune_weights
/tmp/tune_weights > tuned_weights.txt
```

標準エラーへ学習・検証スコアの変更と採用可否、標準出力へC++へ貼り付けられる重み設定を出力する。現在はseed 1～4を学習、5～6を検証に使い、候補数6、各試走2日である。

これは合否だけを見るテストではない。重みを採用する前に、候補数やビーム幅を増やした追加評価と、別seed・`sample_maps/`での検証を行う。
