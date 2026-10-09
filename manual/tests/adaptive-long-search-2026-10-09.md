# 実効時間別ビーム／ALNS配分（2026-10-09）

本選の16×16・24×24・32×32と、3人運用の短時間／中時間／長時間探索に合わせ、主ビームの時間割合と玉数ALNSの試行・展開上限を段階調整した。

## 方針

- 15秒未満は早い合法解と系列数を優先し、ALNS基準上限12試行×800展開を維持する。
- 15秒以上では盤面が大きいほど異なる切断・演算子の試行数を増やし、1試行の展開上限は計算量に合わせて抑える。
- 自動幅の45秒以上は、最終日でビーム65%・後段35%、中間日で70%・30%を基本とする。
- 厳燃料日は補給計画を保護するためビーム85%を維持する。
- 最終日の実効30秒以上では通常2台近傍にALNS枠の3分の2、残りを3台拡張近傍へ割り当てる。
- 公式比較は従来どおり系列数を玉数より優先し、各候補を公式シミュレータで再検証する。

`--adaptive-long-search 0`で従来の固定配分・固定上限へ戻せる。`--quantity-budget-percent N`を明示した場合も比較用の固定配分となる。

## 確認結果

このMacで`-O2 -pthread`、混合交通、単日、1スレッドを使用した。

| 条件 | 構成 | 系列 / 日別系列 / 玉数 | 時間 | ALNS試行 | ALNS展開 |
|---|---|---:|---:|---:|---:|
| 16×16、300ms、seed 1 | 固定 | 12 / 12 / 24 | 300ms | 12 | 4,590 |
| 同上 | 自動 | 12 / 12 / 24 | 300ms | 12 | 3,609 |
| 32×32、45秒、seed 1 | 変更前バイナリ | 24 / 24 / 88 | 40.421秒 | 28 | 35,200 |
| 同上 | 自動 | 24 / 24 / 88 | 42.100秒 | 56 | 84,000 |

300ms比較は`--compare-adaptive-long-search`で交互実行し、短時間では基準上限を増やさず公式得点同点だった。45秒比較は変更前後バイナリを同一seedで順次実行した単発値であり、自動版は系列数と玉数を維持したままALNS試行を2倍、展開を約2.4倍へ増やし、上限45秒内で終了した。このseedでは得点増加はなかったため、全盤面での改善を示す結果ではない。

`solver_benchmark`は手動ビーム割合80%を使うため、45秒値は主に後段配分とALNS上限の効果を切り分けている。自動幅の時間割合と盤面別上限は`udon_alns_test`で境界値を検査し、実行時にはコンソールの次の項目で確認できる。

- `beam-budget-percent-used`
- `quantity-budget-percent-used`
- `udon-alns-attempt-limit`
- `udon-alns-nodes-per-attempt`

## 再現

```sh
make .build/tools/solver_benchmark

.build/tools/solver_benchmark --compare-adaptive-long-search \
  --size 16 --agents 4 --supplies 1 --spots 16 --brands 12 \
  --seeds 1 --repeats 1 --single-day --mixed-traffic --time-ms 300

.build/tools/solver_benchmark --compare-adaptive-long-search \
  --size 32 --agents 7 --supplies 2 --spots 32 --brands 24 \
  --seeds 1 --repeats 1 --single-day --mixed-traffic --time-ms 45000
```

長時間の時間打切り結果はCPU負荷で変動する。本番前の追加調整では複数seed・複数反復を使い、系列数の退行がないことを最優先に判断する。
