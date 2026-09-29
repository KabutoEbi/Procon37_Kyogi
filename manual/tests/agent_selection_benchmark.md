# `agent_selection_benchmark.cpp` の使い方

対象: [`tests/agent_selection_benchmark.cpp`](../../tests/agent_selection_benchmark.cpp)

距離表再利用・候補順序・試走期限の変更後の計測は[ランダムマップでの比較記録](search-selection-results-2026-09-28.md)を参照。

`ProblemCreator`でシード付きのランダムマップを生成し、種別選択後に選ばれた配置で全日を再探索する。16×16は4台・16スポット・12系列、24×24は5台・24スポット・18系列、32×32は7台・32スポット・24系列とする。

```sh
make .build/tools/agent_selection_benchmark
.build/tools/agent_selection_benchmark --size 24 --seeds 8 --selection-ms 5000 --constrained
```

`--constrained`は各日ステップ数と燃料上限をマップ幅に固定する。生成直後のパラメータを使う場合は外す。`--seed-start`で開始シード、`--seeds`で件数を指定する。`--size`を省略すると3サイズを試す。`--selection-ms 0`は相対時間制限なし。

CSV末尾の`completed_scenarios`は選択した候補の試走が完了し、比較に使ったシナリオ数、`timed_out`は時間による比較の打ち切りを示す。短時間の比較では、着手候補数だけでなく、この完了数と選択後の再探索得点を確認する。

CSVの`kinds`は各車の巡回車`P`・補給車`S`を示す。`brands`、`daily_brands`、`udon`は固定幅8／同時行動幅32、全日順調、時間制限なしで再探索した得点である。`holdout_`列は予測交通シナリオの高位条件で再探索した得点である。種別選択中の試走も同じ幅を使い、チーム数・エージェント数から作った低位・中央・高位の予測条件を評価する。`trial_assignments`と`trial_simulations`は締切までに着手した件数で、得点とは区別する。

2026-09-28に`--constrained --selection-ms 5000`で変更前後を比較した。16×16のseed 1～3と32×32のseed 1～3は同点。24×24のseed 1～8では辞書順の得点（系列数、日別系列数、玉数）が3件改善、5件同点、悪化0件だった。改善例はseed 3が`(17, 37, 39)`から`(18, 39, 40)`、seed 4が`(18, 26, 28)`から`(18, 33, 36)`、seed 5が`(17, 35, 36)`から`(17, 36, 37)`である。時間打切りの結果は実行環境やCPU負荷で変わり得る。

同じ3件を未使用の混合交通条件でも確認した。seed 3は`(17, 32, 34)`から`(18, 38, 40)`、seed 4は`(16, 26, 27)`から`(18, 32, 35)`へ改善した。seed 5は`(17, 32, 33)`から`(17, 31, 32)`へ下がった。生成マップとこの交通条件での結果であり、本選マップでの改善は保証しない。
