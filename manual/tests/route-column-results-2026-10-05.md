# 巡回車別の経路候補組合せ探索の比較（2026-10-05）

対象: [`inc/route_column_search.hpp`](../../inc/route_column_search.hpp)、[`tests/solver_benchmark.cpp`](../../tests/solver_benchmark.cpp)

系列数を先に、同数なら獲得玉数を比較する追加探索を、生成問題でON/OFF比較した。`--compare-route-columns`では比較のため5秒未満でも追加探索を実行できる。通常のCLIでは日次予算5秒未満なら実行しない。

| 条件 | 新方式ON | 新方式OFF | 辞書順比較 |
|---|---:|---:|---|
| 24×24、巡回車4台＋補給車1台、24スポット、16系列、混合交通、4日×6シード、各日5秒 | 全試合16系列・日別累計64系列、玉数合計1329 | 全試合16系列・日別累計64系列、玉数合計1319 | ON 2勝4分0敗 |
| 32×32、巡回車2台＋補給車1台、32スポット、24系列、厳燃料・混合交通、初日×2シード、各日5秒 | seed 1: 24系列・30玉、seed 2: 19系列・21玉 | seed 1: 24系列・29玉、seed 2: 17系列・23玉 | ON 2勝0敗 |

再現コマンド:

```sh
.build/tools/solver_benchmark --seeds 6 --repeats 1 --size 24 --agents 5 --spots 24 --brands 16 --days 4 --mixed-traffic --time-ms 5000 --compare-route-columns
.build/tools/solver_benchmark --seeds 2 --repeats 1 --size 32 --agents 3 --spots 32 --brands 24 --single-day --mixed-traffic --tight-fuel --time-ms 5000 --compare-route-columns
```

先行する24×24・seed 1の単日比較では、10秒でも50秒指定でもON/OFFはともに16系列・49玉だった。50秒指定時の探索は約30秒で完了した。短時間の別の4シード比較では改善と退行の両方があったため、通常動作での適用下限を日次予算5秒とした。

これらは保存済みの実戦盤面ではなく、5秒設定での比較が中心である。50秒設定・別の地形、台数、燃料条件での優位性は未確認。系列数が増えて玉数が減る場合もある。全盤面での系列数・玉数の同時改善や最適性は主張しない。

回帰検証では`make test-core`（C++回帰とPython 36件）が成功し、経路組合せの補給・在庫・複数車・大きめの候補プール・末尾訪問追加のテストはASan/UBSanでも通過した。
