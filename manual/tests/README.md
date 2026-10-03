# テストと計測ツール

対象: [`tests/`](../../tests/)

すべてリポジトリ直下から実行する。C++テストにはC++17、`-pthread`、`-Iinc`を指定する。

| ファイル | 用途 |
|---|---|
| [`algorithm_test.cpp`](algorithm_test.md) | 低レベル探索アルゴリズムと最適化の回帰テスト |
| [`model_test.cpp`](model_test.md) | モデルから複数日実行までの総合回帰テスト |
| [`official_qa_test.cpp`](official-qa-coverage-2026-10-01.md) | 公式Q&Aのフェーズ順・生成範囲・補足行動例の回帰テスト |
| [`property_test.cpp`](property_test.md) | ランダム問題と時刻付き補給最短路のプロパティテスト |
| [`sample_maps_test.cpp`](sample_maps_test.md) | `sample_maps/`を使うスモークテスト |
| [`solver_benchmark.cpp`](solver_benchmark.md) | 複数シード・全日程・交通変化・機能別の得点と速度比較 |
| [`agent_selection_benchmark.cpp`](agent_selection_benchmark.md) | ランダム生成マップでの種別選択と全日試走の比較 |
| [`arena_benchmark.cpp`](arena_benchmark.md) | 公開アリーナ問題JSONを使った全日程・混合交通の再現比較 |
| [`tune_weights.cpp`](tune_weights.md) | 評価関数の重み調整 |
| [`visualizer_test.py`](visualizer_test.md) | GUIバックエンドのヘッドレス回帰テスト |
| [`recovery_test.py`](recovery_test.md) | 永続復旧・同日回答更新・同一候補再送・送信時間確保の回帰テスト |
| [`visualizer_gui_test.py`](visualizer_gui_test.md) | Tk上での初期設定・同日再探索・日終了時の更新・手動／自動送信テスト |
| [`visualizer_runtime_test.py`](visualizer_runtime_test.md) | 対応するPython/Tkの選択テスト |
| [`visualizer_simple_server_test.py`](visualizer_simple_server_test.md) | 同梱簡易サーバーと実ソルバーの結合テスト |
| [`solver_options_test.cpp`](solver_options_test.md) | 探索引数の反映と不正値拒否 |

## 回帰テストをまとめて実行

```sh
make -j3 test
```

モデル・探索・引数・公式Q&A・プロパティ・サンプルのC++テストと、実ソルバー連携を含むPythonテストを実行する。どれかが失敗するとmakeも非ゼロで終了する。サンプルJSONが未配置ならエラーになるため、その環境では`make test-core`を使用する。

`make test-all`でGUI表示・同梱簡易サーバー・ASan/UBSanも含めて実行できる。環境要件と個別ターゲットは[`ビルド手順`](../build.md)を参照。

`solver_benchmark`と`tune_weights`は結果を比較・調整するためのツールなので、通常の回帰テストとは分けて実行する。

2026-10-03時点では、32×32向け種別選択、最終日の経路方式比較、理論値表示・評価、補給車経路安定化を含めて`make test`と`make test-sanitize`が成功し、`make test-python`相当のPython 35件もすべて成功している。

GUIバックエンドはPython標準の`unittest`で個別に実行する。

```sh
make test-python
```

補給車の合流候補拡張・燃料制約付き経路の改善結果は[合流改善の比較記録](rendezvous-results-2026-09-30.md)を参照。固定探索量での改善と、300ms制限下に残る退行を分けて記録している。

補給車の目的地先回りと無目的な移動の抑制は[補給車移動の比較記録](supply-movement-results-2026-09-30.md)を参照。

8台時の補給車2台・3台と候補配分の比較は[8台時の種別選択記録](agent-selection-8-results-2026-09-30.md)を参照。

32×32・7台・32スポット・20系列での過少補給対策と混雑・渋滞の比較は[種別選択改善記録](agent-selection-traffic-results-2026-10-02.md)を参照。

32×32・8台・32スポット・20系列・10日で、最終日に複数の経路方式を比較して獲得量を改善した結果は[経路選択改善記録](route-selection-results-2026-10-02.md)を参照。

理論上限を使った種別候補評価、最終日の公式得点優先、迂回合流候補拡張の比較は[理論値差分の改善記録](theoretical-gap-improvements-2026-10-02.md)を参照。

補給車の不要な周回を得点・終端状態を変えず除去した比較は[補給車経路安定化の記録](supply-route-stabilization-2026-10-03.md)を参照。

混雑・渋滞下での古い合流先の再計画と、エージェント選択の再評価は[混合交通での再計画・種別選択記録](mixed-traffic-reroute-results-2026-09-30.md)を参照。

評価重み、状態コピー、短時間探索、および共同割当キャッシュの直列実行高速化は[2026-10-01の探索改善記録](search-improvements-2026-10-01.md)を参照。

混雑・渋滞下の巡回先再割当と、燃料制約時の有効な迂回合流候補の探索は[探索・合流改善記録](mixed-traffic-search-rendezvous-2026-10-01.md)を参照。
