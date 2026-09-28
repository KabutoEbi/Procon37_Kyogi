# テストと計測ツール

対象: [`tests/`](../../tests/)

すべてリポジトリ直下から実行する。C++テストにはC++17、`-pthread`、`-Iinc`を指定する。

| ファイル | 用途 |
|---|---|
| [`algorithm_test.cpp`](algorithm_test.md) | 低レベル探索アルゴリズムと最適化の回帰テスト |
| [`model_test.cpp`](model_test.md) | モデルから複数日実行までの総合回帰テスト |
| [`sample_maps_test.cpp`](sample_maps_test.md) | `sample_maps/`を使うスモークテスト |
| [`solver_benchmark.cpp`](solver_benchmark.md) | 複数シード・全日程・交通変化・機能別の得点と速度比較 |
| [`tune_weights.cpp`](tune_weights.md) | 評価関数の重み調整 |
| [`visualizer_test.py`](visualizer_test.md) | GUIバックエンドのヘッドレス回帰テスト |
| [`recovery_test.py`](recovery_test.md) | 永続復旧・同一回答再送・送信時間確保の回帰テスト |
| [`visualizer_gui_test.py`](visualizer_gui_test.md) | Tk上での初期設定・日次探索・手動／自動送信テスト |
| [`visualizer_runtime_test.py`](visualizer_runtime_test.md) | 対応するPython/Tkの選択テスト |
| [`visualizer_simple_server_test.py`](visualizer_simple_server_test.md) | 同梱簡易サーバーと実ソルバーの結合テスト |
| [`solver_options_test.cpp`](solver_options_test.md) | 探索引数の反映と不正値拒否 |

## 回帰テストをまとめて実行

```sh
make -j3 test
```

モデル・探索・引数・サンプルのC++4テストと、実ソルバー連携を含むPythonテストを実行する。どれかが失敗するとmakeも非ゼロで終了する。サンプルJSONが未配置ならエラーになるため、その環境では`make test-core`を使用する。

`make test-all`でGUI表示・同梱簡易サーバー・ASan/UBSanも含めて実行できる。環境要件と個別ターゲットは[`ビルド手順`](../build.md)を参照。

`solver_benchmark`と`tune_weights`は結果を比較・調整するためのツールなので、通常の回帰テストとは分けて実行する。

GUIバックエンドはPython標準の`unittest`で個別に実行する。

```sh
make test-python
```
