# テストと計測ツール

対象: [`tests/`](../../tests/)

すべてリポジトリ直下から実行し、C++17、`-pthread`、`-Iinc`を指定する。

| ファイル | 用途 |
|---|---|
| [`algorithm_test.cpp`](algorithm_test.md) | 低レベル探索アルゴリズムと最適化の回帰テスト |
| [`model_test.cpp`](model_test.md) | モデルから複数日実行までの総合回帰テスト |
| [`sample_maps_test.cpp`](sample_maps_test.md) | `sample_maps/`を使うスモークテスト |
| [`solver_benchmark.cpp`](solver_benchmark.md) | マクロ行動の速度・探索量比較 |
| [`tune_weights.cpp`](tune_weights.md) | 評価関数の重み調整 |

## 回帰テストをまとめて実行

```sh
for name in model_test algorithm_test sample_maps_test; do
  c++ -std=c++17 -O2 -pthread -Iinc "tests/${name}.cpp" -o "/tmp/${name}"
  "/tmp/${name}"
done
```

`model_test`と`algorithm_test`は成功時に出力しない。`sample_maps_test`は各マップの探索結果を表示する。3つとも終了コード0なら成功である。

`solver_benchmark`と`tune_weights`は結果を比較・調整するためのツールなので、通常の回帰テストとは分けて実行する。
