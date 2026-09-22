# `solver_benchmark.cpp` の使い方

対象: [`tests/solver_benchmark.cpp`](../../tests/solver_benchmark.cpp)

ランダム生成した複数サイズの問題で、マクロ行動の有無による探索結果と速度を比較する簡易ベンチマークである。

## 実行

```sh
c++ -std=c++17 -O2 -pthread -Iinc tests/solver_benchmark.cpp -o /tmp/solver_benchmark
/tmp/solver_benchmark > benchmark.csv
```

出力は次のCSV形式である。

```text
size,seed,macros,brands,udon,ms,nodes,valid
```

- `macros`: マクロ行動を有効にしたか
- `brands`: 試合を通した取得系列数
- `udon`: 当日の取得玉数
- `ms`: 探索経過時間
- `nodes`: 展開ノード数
- `valid`: 生成計画を再検証した結果

各探索は1スレッド、相対時間制限300 msで実行する。性能値はCPU負荷などに左右されるため、同じ環境・同じビルド設定で比較する。
