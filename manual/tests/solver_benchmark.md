# `solver_benchmark.cpp` の使い方

対象: [`tests/solver_benchmark.cpp`](../../tests/solver_benchmark.cpp)

複数サイズ・複数シード・複数回の実行で、探索改善の有無による全日程の得点と速度を比較する。種別は巡回車と補給車1台に固定し、種別選択の差を混ぜない。

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
```

出力は次のCSV形式である。

```text
size,seed,repeat,variant,day,brands,daily_brands,udon,ms,nodes,cache_hits,assignment_computations,valid,plan_hash,assignment_ns,flow_ns,rendezvous_ns
```

- `variant`: 比較する機能構成
- `brands`: その日までの試合取得系列数
- `daily_brands`: その日までの日別系列数の合計
- `udon`: その日までの総取得玉数
- `ms`: 探索経過時間
- `nodes`: 展開ノード数
- `cache_hits`, `assignment_computations`: 割当のキャッシュ命中数と計算数
- `valid`: 計画を前日得点込みで再シミュレーションし、終了状態の一致とフォールバック未使用を確認した結果。不一致時は終了コード1
- `plan_hash`: エージェントごとの行動列・長さの64bit指紋。速度改善前後の同一設定で、得点・展開数と併せて比較する
- `assignment_ns`, `flow_ns`, `rendezvous_ns`: `--profile`時の処理時間。未指定時は0。計測範囲が重複するため加算しない

既定は8/16/24/32の4サイズ、seed 1〜8、各3回、4日間、1スレッド、日次300msで実行する。`--seeds`、`--repeats`、`--size`、`--time-ms`で変更できる。`--single-day`は初日のみを比較し、翌日配置の評価も初日終了で打ち切る。

`--fixed-work`は時間制限と幅の自動調整を無効にし、ビーム幅32・同時行動幅128で最後まで探索する。高速化前後の実行ファイルを同じ引数で実行し、`plan_hash`・得点・展開数が一致した行の`ms`を比較する。時間制限付きの比較は探索量が変わるため、これと分けて評価する。

各問題で`baseline`と`--variant`の構成を比較し、実行順を交互に入れ替える。`baseline`は今回の巡回先・合流の再割当、迂回合流、2店先読み、割当キャッシュをすべて無効にした構成で、前回までのマクロ探索を使用する。既定の`improved`はすべて有効。`no-cache`、`no-coordination`、`no-detour`、`no-lookahead`、`no-macros`はそれぞれ1機能を無効にする。`no-patrol-replan`は共同割当変更時の巡回マクロ再計画を、`limited-detour-scan`は燃料制約時の到達可能候補まで続ける迂回探索を無効にする。`no-hard-nextday`は最終日前日の遠隔一意系列への準備を、`no-route-portfolio`は最終日の経路方式比較を無効にする。`replan-2`、`replan-4`、`replan-8`は指定イベント間隔で常にマクロ経路を再検討する。機能別の効果は、同じ問題・同じ回数の`improved`行とも照合する。

標準エラーへ試合単位の得点合計、時間の中央値・最大値、展開数、辞書順（試合系列→日別系列累計→玉数）での勝ち・同点・負けを出力する。性能値はCPU負荷などに左右されるため、同じ環境・同じビルド設定で比較する。固定シードでも時間打切りの結果は変動し、生成問題での改善が本番問題での改善を保証するものではない。

2026-09-30の合流改善について、固定探索量と1日300ms制限の変更前後比較を[合流改善の比較記録](rendezvous-results-2026-09-30.md)にまとめた。

同日の補給車先回り移動については[補給車移動の比較記録](supply-movement-results-2026-09-30.md)にまとめた。

## 重み・途中解・変更前後の比較（2026-10-01）

- `--current-only`: `--variant`の構成だけを実行する。異なるバイナリの同条件比較用。
- `--seed-start N`: seedの開始値。学習・検証と重ならない問題集合を作る。
- `--agents N`, `--spots N`, `--brands N`, `--days N`: 生成する台数、スポット数、系列数、日数を上書きする。
- `--legacy-weights`: 燃料制約時の4つの重み補正を無効にする。
- `--no-completion`: 途中解の未実行マクロ経路の完了を無効にする。
- `--tight-fuel`: 燃料上限を初日ステップ数の半分にする。

```sh
.build/tools/solver_benchmark --current-only --seed-start 3001 --seeds 3 \
  --repeats 2 --time-ms 300 --mixed-traffic > current.csv
# 高速化だけの比較では、重みと途中解保存方式を変更前に合わせる
.build/tools/solver_benchmark --current-only --legacy-weights --no-completion \
  --fixed-work --seeds 3 --repeats 1 --mixed-traffic > speed.csv
```

`--current-only`では別バイナリ間の実行順を自動交替しない。比較側で順序を変えた反復も行う。
結果は[改善記録](search-improvements-2026-10-01.md)を参照。

混合交通での巡回マクロ再計画と燃料制約付き合流候補の比較は
[探索・合流改善記録](mixed-traffic-search-rendezvous-2026-10-01.md)を参照。

32×32・8台・32スポット・20系列・10日での`no-route-portfolio`との比較、適用条件、探索時間と展開数は[経路選択改善記録](route-selection-results-2026-10-02.md)を参照する。
