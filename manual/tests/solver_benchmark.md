# `solver_benchmark.cpp` の使い方

対象: [`tests/solver_benchmark.cpp`](../../tests/solver_benchmark.cpp)

`--no-coordinated-refinement` で終端再配置・先回り補給・時間配分の追加を切り分けられる。共通の燃料付き翌日評価は残るので、完全な旧版比較には変更前のバイナリを使用する。

## 保存済みの日次状態を再探索

`saved_day_benchmark` は過去の `endsAt` だけを無効化して、保存時点の位置・燃料・交通・得点から探索する。`runtime` への書込みやサーバー送信は行わない。出力計画はシミュレータで再検証する。

```sh
make .build/tools/saved_day_benchmark
.build/tools/saved_day_benchmark \
  runtime/logs/20261003-140003-405057000-get-setting.json \
  runtime/state-day3.json runtime/score-day3.json 50000
```

引数は設定、開始状態、前日までの得点（初日は `-` 可）、時間上限ms（省略時50000）、追加改善の有効化0/1（省略時既定値）。設定は現在の `runtime/setting.json` ではなく、その試合の保存設定を指定すること。

複数サイズ・複数シード・複数回の実行で、探索改善の有無による全日程の得点と速度を比較する。既定は補給車1台に固定し、`--supplies N`で補給車数だけを変更できるため、種別選択の差を混ぜず経路探索を比較できる。

2026-09-27の計測結果は[比較記録](benchmark-results-2026-09-27.md)を参照。

2026-09-28の距離表再利用と種別選択の変更は[ランダムマップでの比較記録](search-selection-results-2026-09-28.md)を参照。

2026-09-29の日開始取得、道路状態による完了不能移動の除外、探索パラメータ候補の比較は[ランダムマップ探索・改善記録](search-results-2026-09-29.md)を参照。

同日の種別選択・探索内部の高速化は[高速化記録](algorithm-speed-results-2026-09-29.md)を参照。

同日の[高速化前後の比較](speed-results-2026-09-27.md)では、固定探索量で計画・得点を保った速度改善と、時間制限下で探索量が変わる効果を分けて記録している。

## 実行

```sh
c++ -std=c++17 -O2 -pthread -Iinc tests/solver_benchmark.cpp -o /tmp/solver_benchmark
/tmp/solver_benchmark > benchmark.csv
# 短い動作確認
/tmp/solver_benchmark --seeds 2 --repeats 1 --size 16 --time-ms 300
# 日ごとに異なる混雑・渋滞を与える
/tmp/solver_benchmark --seeds 4 --repeats 2 --mixed-traffic > mixed.csv
# キャッシュを外した場合の比較
/tmp/solver_benchmark --variant no-cache --seeds 4 --repeats 2 > no-cache.csv
# 時間制限を外し、同じ幅で最後まで探索する速度比較
/tmp/solver_benchmark --fixed-work --seeds 3 --repeats 2 > fixed.csv
# ボトルネックの計測（速度比較とは分けて実行）
/tmp/solver_benchmark --fixed-work --profile --seeds 2 --repeats 1 > profile.csv
# 厳燃料日の残燃料評価（除数10対20）だけを交互比較
/tmp/solver_benchmark --compare-tight-future-fuel --tight-fuel \
  --size 24 --seeds 16 --repeats 1 --time-ms 300
# 燃料切れ巡回車への直接救援だけを交互比較
/tmp/solver_benchmark --compare-stranded-rescue --agents 7 --supplies 2 \
  --size 32 --days 10 --mixed-traffic --time-ms 5000
# 実効時間別のビーム／ALNS自動配分を固定配分と交互比較
/tmp/solver_benchmark --compare-adaptive-long-search \
  --size 32 --agents 7 --supplies 2 --spots 32 --brands 24 \
  --single-day --mixed-traffic --time-ms 45000
```

出力は次のCSV形式である。

```text
size,seed,repeat,variant,day,brands,daily_brands,udon,ms,nodes,cache_hits,assignment_computations,supply_moves,supply_backtracks,removed_supply_loops,valid,plan_hash,assignment_ns,flow_ns,rendezvous_ns,repair_attempts,repair_expanded,repair_improvements,alns_attempts,alns_expanded,alns_improvements,route_columns_generated,route_columns_master_nodes,route_columns_improvements,day_beam_candidates,day_beam_expanded,day_beam_complete,supply_move_steps,supply_wait_steps,supply_refuels,supplied_fuel,supply_idle_after_last_refuel
```

- `variant`: 比較する機能構成
- `brands`: その日までの試合取得系列数
- `daily_brands`: その日までの日別系列数の合計
- `udon`: その日までの総取得玉数
- `ms`: 探索経過時間
- `nodes`: 展開ノード数
- `cache_hits`, `assignment_computations`: 割当のキャッシュ命中数と計算数
- `supply_moves`: 補給車の移動回数
- `supply_backtracks`: 補給車が直前の移動方向へ即座に戻った回数
- `removed_supply_loops`: 最終状態一致を確認して待機へ置換した補給車の周回区間数
- `valid`: 計画を前日得点込みで再シミュレーションし、終了状態の一致とフォールバック未使用を確認した結果。不一致時は終了コード1
- `plan_hash`: エージェントごとの行動列・長さの64bit指紋。速度改善前後の同一設定で、得点・展開数と併せて比較する
- `assignment_ns`, `flow_ns`, `rendezvous_ns`: `--profile`時の処理時間。未指定時は0。計測範囲が重複するため加算しない
- `repair_attempts`, `repair_expanded`, `repair_improvements`: 系列救済の試行数、状態展開数、採用数。採用数は全車探索前のビーム計画に対する改善数であり、従来の全車探索より勝った試合数とは異なる
- `alns_attempts`, `alns_expanded`, `alns_improvements`: 系列マスク固定ALNSの試行数、状態展開数、玉数改善の採用数
- `route_columns_generated`, `route_columns_master_nodes`, `route_columns_improvements`: 巡回車別の経路候補数、在庫制約付き組合せ探索の訪問ノード数、採用改善数
- `day_beam_candidates`, `day_beam_expanded`, `day_beam_complete`: 日跨ぎ候補数、将来の日次探索回数、全候補比較完了
- `supply_move_steps`, `supply_wait_steps`: 補給車が移動中・待機中だったステップ数
- `supply_refuels`, `supplied_fuel`: 実際に燃料が回復した巡回車の延べ台数と供給燃料量
- `supply_idle_after_last_refuel`: 各補給車について最後の実補給から日終了までのステップ数の合計。補給しなかった車は1日分を数える

`no-quantity-upgrades`は堅牢な2店先読み、取得スポット多様化、共有合流、拡張ALNSポートフォリオをまとめて無効化する。`no-robust-route`、`no-quantity-diversity`、`no-shared-rendezvous`、`no-expanded-alns`は各機能のアブレーションに使う。

`--quantity-upgrades`で新しい5項目を有効化する（CLI本体と違い値なしのベンチ用フラグ）。`--no-day-beam`は日跨ぎ先読みを無効化する。`--keep-beam-iterations`は系列上限での早期終了を無効化する。`--quantity-budget-percent N`（1〜90）は玉数用の予約割合を変更し、時間配分の切り分けに使う。これら3つの調整は`--quantity-upgrades`と併用する。

既定は8/16/24/32の4サイズ、seed 1〜8、各3回、4日間、1スレッド、日次300msで実行する。`--seeds`、`--repeats`、`--size`、`--time-ms`で変更できる。`--single-day`は初日のみを比較し、翌日配置の評価も初日終了で打ち切る。

`--fixed-work`は時間制限と幅の自動調整を無効にし、ビーム幅32・同時行動幅128で最後まで探索する。高速化前後の実行ファイルを同じ引数で実行し、`plan_hash`・得点・展開数が一致した行の`ms`を比較する。時間制限付きの比較は探索量が変わるため、これと分けて評価する。

`--compare-adaptive-long-search`は、盤面・実効時間別のALNS上限と後段配分を使う`adaptive-long-search`、従来の固定上限・固定配分を使う`fixed-long-search`を交互に実行する。時間打切りの揺れを減らすため複数seed・複数反復で判断し、短時間、35秒級、55秒級を分けて記録する。

各問題で`baseline`と`--variant`の構成を比較し、実行順を交互に入れ替える。`baseline`は今回の巡回先・合流の再割当、迂回合流、2店先読み、割当キャッシュをすべて無効にした構成で、前回までのマクロ探索を使用する。既定の`improved`はすべて有効。`--compare-detour-aware-sa`は利益÷迂回量による低頻度の狙い撃ち、`--compare-differential-insertion`は挿入位置の局所差分評価、`--compare-reachability-rarity`は到達可能巡回車数による希少系列ポートフォリオを、それぞれ直前方式と対比較する。`no-cache`、`no-coordination`、`no-detour`、`no-lookahead`、`no-macros`はそれぞれ1機能を無効にする。`no-patrol-replan`は共同割当変更時の巡回マクロ再計画を、`no-supply-cleanup`は探索後の補給車周回除去を、`wandering-supply`は合流未割当時の自由移動を有効にする。`unbounded-supply-wait`は補給車の待機上限を0にして旧方式へ戻す。`legacy-supply-schedules`は経路候補組合せの固定待機案を1件、時刻付き合流を最大2回へ戻す比較用設定である。`terminal-wait-fill`は実験的な末尾待機活用を有効化し、`no-transit-pickup-ranking`は32×32の経路ラベルにおける通過取得順位を無効化する。`limited-detour-scan`は燃料制約時の到達可能候補まで続ける迂回探索を無効にする。`no-hard-nextday`は最終日前日の遠隔一意系列への準備を、`no-route-portfolio`は最終日の経路方式比較を無効にする。`replan-2`、`replan-4`、`replan-8`は指定イベント間隔で常にマクロ経路を再検討する。機能別の効果は、同じ問題・同じ回数の`improved`行とも照合する。

標準エラーへ試合単位の得点合計、時間の中央値・最大値、展開数、辞書順（試合系列→日別系列累計→玉数）での勝ち・同点・負けを出力する。性能値はCPU負荷などに左右されるため、同じ環境・同じビルド設定で比較する。固定シードでも時間打切りの結果は変動し、生成問題での改善が本番問題での改善を保証するものではない。

2026-09-30の合流改善について、固定探索量と1日300ms制限の変更前後比較を[合流改善の比較記録](rendezvous-results-2026-09-30.md)にまとめた。

同日の補給車先回り移動については[補給車移動の比較記録](supply-movement-results-2026-09-30.md)にまとめた。

## 重み・途中解・変更前後の比較（2026-10-01）

- `--current-only`: `--variant`の構成だけを実行する。異なるバイナリの同条件比較用。
- `--compare-brand-repair`: 連鎖再計画だけを切り替えた`no-brand-repair`と`brand-repair`を同じ時間予算・問題で交互に実行する。他の探索機能は両方で有効。`--current-only`との併用は不可。
- `--compare-udon-alns`: ALNSだけを切り替えた`no-udon-alns`と`udon-alns`を同じ条件で交互に実行する。`--compare-brand-repair`との併用は不可。
- `--compare-udon-sa`: 同じ玉数ALNSで焼きなましだけを切り替えた`no-udon-sa`と`udon-sa`を交互に実行する。
- `--variant no-arena-guidance`: SAの追加・置換スポットを一様乱数へ戻し、公開得点から導いた在庫重み付き誘導を切り分ける。
- `--variant arena-stock-guidance`, `arena-both-guidance`: 在庫重みだけ、または在庫重みと実験的な近傍確率変更を有効にする。
- `--variant no-joint-destroy-repair`: SA前の複数巡回車・補給車共同再構築だけを無効化する。
- `--seed-start N`: seedの開始値。学習・検証と重ならない問題集合を作る。
- `--agents N`, `--supplies N`, `--spots N`, `--brands N`, `--days N`: 生成する台数、補給車数、スポット数、系列数、日数を上書きする。補給車数は台数未満でなければならない。
- `--legacy-weights`: 燃料制約時の4つの重み補正を無効にする。
- `--no-completion`: 途中解の未実行マクロ経路の完了を無効にする。
- `--tight-fuel`: 燃料上限を初日ステップ数の半分にする。
- `--compare-tight-future-fuel`: 厳燃料日の残燃料評価だけを切り替え、除数10の既定版と従来相当の20を交互に比較する。`tight-future-fuel-5/10/20/40`は係数調整用variant。

```sh
.build/tools/solver_benchmark --current-only --seed-start 3001 --seeds 3 \
  --repeats 2 --time-ms 300 --mixed-traffic > current.csv
# 高速化だけの比較では、重みと途中解保存方式を変更前に合わせる
.build/tools/solver_benchmark --current-only --legacy-weights --no-completion \
  --fixed-work --seeds 3 --repeats 1 --mixed-traffic > speed.csv
```

`--current-only`では別バイナリ間の実行順を自動交替しない。比較側で順序を変えた反復も行う。

`--compare-route-columns`は`no-route-columns`と`improved`を同じ生成問題・予算で交互に実行する。この比較モードでは5秒未満でも新方式を評価できるよう、適用下限を0msへ変更する。通常のCLIでの既定適用条件とは異なる。個別構成の比較には`--variant no-route-columns`も使える。結果と限界は[経路候補組合せの検証記録](route-column-results-2026-10-05.md)を参照する。

`--compare-stranded-rescue`は他の経路候補探索を共通にしたまま、`maxStrandedRescueSchedules=0`の`no-stranded-rescue`と既定値2の`improved`を交互に実行する。

系列救済型の連鎖再計画だけを比較する例:

```sh
.build/tools/solver_benchmark --compare-brand-repair \
  --seeds 4 --repeats 2 --days 4 --mixed-traffic --tight-fuel --time-ms 300
```

`--fixed-work`で比較した場合も局所補完の追加計算量は異なるため、同じ総探索量や同じ実行時間の比較ではない。効果は時間制限付きの結果とも照合する。

実装と2026-10-03の比較結果は[系列救済の検証記録](brand-repair-results-2026-10-03.md)を参照。
結果は[改善記録](search-improvements-2026-10-01.md)を参照。

混合交通での巡回マクロ再計画と燃料制約付き合流候補の比較は
[探索・合流改善記録](mixed-traffic-search-rendezvous-2026-10-01.md)を参照。

32×32・8台・32スポット・20系列・10日での`no-route-portfolio`との比較、適用条件、探索時間と展開数は[経路選択改善記録](route-selection-results-2026-10-02.md)を参照する。
