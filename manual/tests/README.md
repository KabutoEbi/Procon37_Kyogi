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
| [`beam_width_probe.cpp`](beam-width-tuning-2026-10-04.md) | 固定ビーム幅で日次探索が自然完了するまでの時間計測 |
| [`agent_selection_benchmark.cpp`](agent_selection_benchmark.md) | ランダム生成マップでの種別選択と全日試走の比較 |
| [`arena_benchmark.cpp`](arena_benchmark.md) | 公開アリーナ問題JSONを使った全日程・混合交通の再現比較 |
| [`arena-score-analysis-1905.md`](arena-score-analysis-1905.md) | 公開試合1905までの得点傾向、AHC解法、SA挿入差分評価の採否 |
| [`arena-score-analysis-1918.md`](arena-score-analysis-1918.md) | 公開試合1918までの上位得点、反復深化、系列上限なし救済の検証 |
| [`arena-score-analysis-1922-1926.md`](arena-score-analysis-1922-1926.md) | 1922・1926の日別系列不足と翌日急短縮への終端配置改善 |
| [`arena-score-analysis-1931.md`](arena-score-analysis-1931.md) | 公開98試合、巡回台数下限予測、追加SAの比較、ローカル無制限探索修正 |
| [`first-round-16-results-2026-10-10.md`](first-round-16-results-2026-10-10.md) | 一回戦16×16限定の3巡回車同時再配分、通常・厳燃料比較 |
| [`arena-score-analysis-1937-16.md`](arena-score-analysis-1937-16.md) | 最新16×16の上位目標、最終日追加経路候補、退行した案の撤回 |
| [`arena-score-analysis-1969-16.md`](arena-score-analysis-1969-16.md) | 1969の系列・玉数不足、16×16限定の玉数優先経路と系列フォールバック |
| `analyze_small_board_history.py` / `small_board_api_benchmark.py` | 公開詳細の16×16ラウンド集計と1機能の対比較 |
| `analyze_arena_history.py` | API詳細キャッシュのラウンド重複除去、回収率学習、未学習ラウンド検証 |
| [`takamatsu-pamphlet-search-analysis.md`](takamatsu-pamphlet-search-analysis.md) | 高松大会パンフレットのビーム・SA解法との照合と希少系列評価の不採用理由 |
| [`saved_day_benchmark.cpp`](coordinated-refinement-results-2026-10-07.md) | 保存済みの設定・当日状態・得点から1日を再探索する診断ツール |
| [`runtime_match_benchmark.cpp`](arena-score-analysis-1969-16.md) | 保存済みの全日交通を候補自身の状態遷移で再生する読取専用比較ツール |
| [`tune_weights.cpp`](tune_weights.md) | 評価関数の重み調整 |
| [`visualizer_test.py`](visualizer_test.md) | GUIバックエンドのヘッドレス回帰テスト |
| [`recovery_test.py`](recovery_test.md) | 永続復旧・同日回答更新・同一候補再送・送信時間確保の回帰テスト |
| [`visualizer_gui_test.py`](visualizer_gui_test.md) | Tk上での初期設定・同日再探索・日終了時の更新・手動／自動送信テスト |
| [`visualizer_runtime_test.py`](visualizer_runtime_test.md) | 対応するPython/Tkの選択テスト |
| [`visualizer_simple_server_test.py`](visualizer_simple_server_test.md) | 同梱簡易サーバーと実ソルバーの結合テスト |
| [`solver_options_test.cpp`](solver_options_test.md) | 探索引数の反映と不正値拒否 |
| `spot_route_lns_test.cpp` | スポット単位ビーム、巡回車間の訪問移送・交換、中間日終点変更、経路候補組合せの合法性と非退行 |
| `brand_repair_test.cpp` | 役割交換・補給・巻き戻し・固定行動の系列救済テスト。詳細は[系列救済](../inc/brand_repair.md) |
| `udon_alns_test.cpp` | 系列マスク固定、玉数改善、ランダム問題の非退行・合法性テスト |
| `quantity_search_test.cpp` | 途中取得・時刻付き補給・日跨ぎ候補・比較中断の回帰テスト |

## 回帰テストをまとめて実行

```sh
make -j3 test
```

モデル・探索・系列最大化・連鎖再計画・玉数ALNS・引数・公式Q&A・プロパティ・サンプルのC++テストと、実ソルバー連携を含むPythonテストを実行する。どれかが失敗するとmakeも非ゼロで終了する。サンプルJSONが未配置ならエラーになるため、その環境では`make test-core`を使用する。

`make test-all`でGUI表示・同梱簡易サーバー・ASan/UBSanも含めて実行できる。環境要件と個別ターゲットは[`ビルド手順`](../build.md)を参照。

`solver_benchmark`と`tune_weights`は結果を比較・調整するためのツールなので、通常の回帰テストとは分けて実行する。

2026-10-04時点では、系列救済型の連鎖再計画、系列固定の玉数ALNS、32×32向け種別選択、最終日の経路方式比較、理論値表示・評価、補給車経路安定化を含めて`make test`と`make test-sanitize`が成功し、Python 36件もすべて成功している。アプリアイコン読込を含むGUI実画面1件も成功している。

2026-10-05の経路候補組合せ追加後は`make test-core`（C++回帰、Python 36件）と`spot_route_lns_test`のASan/UBSan実行が成功した。今回の変更について`make test`全体やGUI実画面を再実行したという意味ではない。

2026-10-06には本選3区分の生成と条件外の拒否を追加検証し、`make test`（公開3マップとPython 37件を含む）が成功した。公開試合APIに基づく候補保持の比較は[記録](arena-api-results-2026-10-06.md)、AHCを参考にした経路探索の比較は[記録](ahc-inspired-search-2026-10-06.md)を参照する。

2026-10-07には翌日終端配置、在庫再配分、日跨ぎ候補、16×16の適応的系列救済、大盤面の終端固定玉数候補を追加した。`make test`はC++ 11テスト、公開3サイズ、Python 38件が成功。`make test-sanitize`はC++ 10テストがASan/UBSanで成功した。[協調改善](coordinated-refinement-results-2026-10-07.md)と[公開得点追跡改善](arena-score-guided-improvements-2026-10-07.md)に条件と制約を記録している。

2026-10-08には公開試合1539～1671を再集計し、5台問題の中心候補を4巡回+1補給へ変更した。公開設定2件と生成24×24の3シードで補給2台構成を上回り、`make test-core`と`make test-sanitize`が成功した。集計方法、比較値、補給0台を既定化しなかった理由は[公開試合1671までの分析](arena-score-analysis-1671.md)を参照する。

同日、AHC059・AHC036・AHC027の上位解法を参照して閾値受理、省燃料経路候補、末尾挿入の予算予約を比較した。探索予算を奪って退行した3案は撤回し、通常盤面へ影響しない厳燃料日の残燃料タイブレークだけを採用した。[条件・結果・不採用理由](ahc-inspired-search-2026-10-08.md)を参照する。

2026-10-09には3人運用を追加し、`make test-core`でPython 53件を含む回帰テスト、`make test-gui`で実画面1件が成功した。その後、現在の担当プリセットを15／35／45秒とし、担当1・2はビーム系、担当3はSAへ自動設定する回帰を追加した。問題・状態の公開待ち、担当1の全自動、追走端末の手動送信、口頭3値の取込も検査する。公式実サーバーへの送信試験ではない。

同日、15／35／55秒運用に合わせてビームと後段改善の時間配分、盤面別の玉数ALNS上限を調整した。短時間3サイズでは系列数の退行がなく、32×32の玉数が1増加した。32×32・45秒計測では同点のままALNS試行数が28から56、展開数が35,200から84,000へ増え、締切内で長時間枠を使用した。[条件と結果](adaptive-long-search-2026-10-09.md)を参照する。

公開試合#1809では全チームが系列満点で、超10Gは首位と186玉差だった。同一設定の固定mixed交通では、従来の短い全日種別試走が選んだ5巡回+2補給を、燃料余裕時の4日集中試走による6巡回+1補給が92玉上回った。APIから確認できる範囲、上位解法の推定、変更条件は[公開試合1809の分析](arena-score-analysis-1809.md)に記録した。

公開試合#1813・#1815では超10Gと首位の差がそれぞれ40玉、33玉で、両方とも系列指標は満点だった。#1813の固定mixed交通では6巡回+1補給の`23/89/156`に対し、候補を深く評価して選んだ5巡回+2補給が`23/92/166`となった。候補数を6へ絞る理由と#1815の理論上限・経路改善課題は[公開試合1813・1815の分析](arena-score-analysis-1813-1815.md)に記録した。

公開試合#1367〜#1866の有効499試合を集計し、首位が499試合すべてで試合系列満点、491試合で日別系列満点となる傾向から、brain2へ高在庫スポット選択と巡回車2〜3台・補給車の共同destroy/repairを入れた。24×24生成4試合の在庫重み切り分けでは`851→856`、共同前段は#1866と32×32生成1試合で同点だった。取得範囲、推定の限界、適用下限と検証条件は[公開試合1866までの分析](arena-score-analysis-1866.md)に記録した。

2026-10-10には公開試合#1867〜#1905とAHC059・AHC070の解法を照合し、SAの挿入位置を交通込み前後辺の厳密な差分評価へ変更した。`make -j4 test-core`、`make -j4 test-sanitize`、本番用`-O3 -DNDEBUG -Werror`ビルドが成功した。[公開試合1905までの分析](arena-score-analysis-1905.md)を参照する。同日、高松大会パンフレットのビーム・SA採用校も確認し、既存の2-opt、担当交換、翌日評価、複数日ビームとの重複を整理した。到達可能巡回車数による希少系列評価は8 seedで退行したため既定無効とした。[パンフレット解法分析](takamatsu-pamphlet-search-analysis.md)を参照する。

同日、公開試合#1918までを再分析し、長時間ビームを目標幅の4分の1から反復深化する方式と、系列補完の初回上限後に残時間で上限なし救済を行う方式を追加した。#1922・#1926の日別系列不足に対しては、翌日が約3分の2以下へ短くなる日に遠隔一意系列へ接近する終端評価を追加した。担当2の安定終了後も系列救済を実行し、日跨ぎ候補探索を実験的玉数改善群から分離して通常有効にした。`make -j4 test-core`はPython 57件を含めて成功し、`make -j4 test-sanitize`、`git diff --check`、本番用`-O3 -DNDEBUG -Werror`ビルドも成功した。[1918分析](arena-score-analysis-1918.md)と[1922・1926分析](arena-score-analysis-1922-1926.md)を参照する。

種別選択時間は順位へ影響しないため、GUIは相対上限なしで設定取得時に確定する60／90／120秒の受付枠から、通信timeout・時計ずれ・保存処理の余裕だけを引いた時点まで試走する。#1813の再比較後、既定候補数は各候補を深く評価できる6件へ変更した。受付枠を相対上限へ変換する回帰とGUIの`-O3 -DNDEBUG`本番ビルド検査を追加した。

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

未獲得系列に対する巻き戻し・役割交換・補給車連携の比較は[連鎖再計画の検証記録](brand-repair-results-2026-10-03.md)を参照。

系列マスクを完全に固定したALNSの玉数改善は[玉数ALNSの検証記録](udon-alns-results-2026-10-04.md)を参照。

巡回車別の経路候補組合せの5秒ON/OFF比較とその限界は[2026-10-05の検証記録](route-column-results-2026-10-05.md)を参照。

16系列・4日64系列を維持したスポット再探索と保存済み対局の比較は[玉数再探索の検証記録](quantity-portfolio-results-2026-10-04.md)を参照。

経路の次善候補、取得履歴の探索多様化、同一地点の共有合流による玉数改善は[経路・探索・合流改善記録](quantity-routing-results-2026-10-04.md)を参照。

ビーム幅を固定した32×32・8台の完走時間と、50秒前後の幅の選定は[50秒探索のビーム幅計測](beam-width-tuning-2026-10-03.md)を参照。

スポット再探索を含む現行コードの24×24・5台および32×32・8台の幅選定は[現行ソルバーの50秒ビーム幅](beam-width-tuning-2026-10-04.md)を参照。

15／35／55秒の実効時間別配分とALNS上限は[長時間探索の適応配分](adaptive-long-search-2026-10-09.md)を参照。

系列数を固定した焼きなまし、最良解の非退行保証、短時間比較は[玉数SA-ALNS](simulated-annealing-results-2026-10-09.md)を参照。

混雑・渋滞下での古い合流先の再計画と、エージェント選択の再評価は[混合交通での再計画・種別選択記録](mixed-traffic-reroute-results-2026-09-30.md)を参照。

評価重み、状態コピー、短時間探索、および共同割当キャッシュの直列実行高速化は[2026-10-01の探索改善記録](search-improvements-2026-10-01.md)を参照。

混雑・渋滞下の巡回先再割当と、燃料制約時の有効な迂回合流候補の探索は[探索・合流改善記録](mixed-traffic-search-rendezvous-2026-10-01.md)を参照。
