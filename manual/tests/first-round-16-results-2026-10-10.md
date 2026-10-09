# 一回戦16×16向け調整（2026-10-10）

対象は公式告知の16×16・4エージェント・16スポット・10～14系列。
日数、燃料、ステップ、在庫、交通条件は当日公開のため固定しない。

## 採用した変更

- 16×16の通常玉数ALNSで、同時に解放する巡回車の上限を2台から3台へ変更。
  巡回3台＋補給1台構成の巡回経路・在庫をまとめて再配分できる。
  試行数・時刻上限は増やさず、公式得点の系列優先判定を維持する。
- `smallBoardJointRefinement=true`が既定。`--small-board-joint-refinement 0`で
  従来の2台上限へ戻せる。24×24・32×32と長方形には適用しない。
- GUIのランダム生成初期値を16×16・4台・16スポット・14系列へ変更。
  未公開の日数は従来どおり10日。API取得問題は設定JSONの値をそのまま使用する。
- 種別選択は巡回3台＋補給1台を中心とし、燃料条件と試走得点で他構成も比較する。
  上位得点モデルは巡回台数の下限目安であり、種別を決め打ちしない。

## 固定探索量での対比較

C++17・`-O2`・1スレッド、生成4日、巡回3台＋補給1台、混合交通。
ビーム幅32／同時幅128、時間打切り・日跨ぎビームなし。
実行順はseedごとに交互。表は試合系列／日別系列累計／玉数の合計。

| 条件 | 従来2台上限 | 新3台上限 | 公式得点順の勝／分／敗 |
|---|---|---|---|
| 14系列、seed 1～8 | 112 / 447 / 956 | 112 / 448 / 955 | 1 / 7 / 0 |
| 10系列、燃料を初日ステップの半分に制限、seed 1～4 | 40 / 158 / 382 | 40 / 158 / 385 | 1 / 3 / 0 |

14系列条件は系列優先では改善だが、玉数だけを見ると合計1玉減る。
日別系列満点は448で、従来の不足1系列を解消した。
厳燃料条件では系列数を維持して3玉増えた。
全本番問題の最適性・上位チーム超えを保証する結果ではない。

```sh
make .build/tools/solver_benchmark
.build/tools/solver_benchmark --size 16 --agents 4 --spots 16 --brands 14 \
  --seeds 8 --repeats 1 --mixed-traffic --fixed-work --no-day-beam \
  --compare-small-board-joint-refinement
.build/tools/solver_benchmark --size 16 --agents 4 --spots 16 --brands 10 \
  --seeds 4 --repeats 1 --mixed-traffic --tight-fuel --fixed-work --no-day-beam \
  --compare-small-board-joint-refinement
```

## 時間制限付き比較

15秒/日・14系列・混合交通・単日seed 1～2でも、双方
`28/28/71`（0勝2分0敗）。各試合は約10.8秒以内に終了した。
これは60秒本番枠や複数日の非退行を証明するものではない。

```sh
.build/tools/solver_benchmark --size 16 --agents 4 --spots 16 --brands 14 \
  --seeds 2 --repeats 1 --mixed-traffic --single-day --no-day-beam \
  --time-ms 15000 --compare-small-board-joint-refinement
```

## 不採用の追加探索

中間日にも終端位置・燃料を完全固定した追加3台ポートフォリオを実行する案は、
14系列8 seedの固定探索量で双方`112/447/956`、1秒/日4 seedでも双方
`56/224/503`だった。探索量だけ増えたため採用しない。
既存の最終日追加探索と24×24以上の追加探索は維持する。

## 検証

`make -j2 test-core`が成功。C++の合法性・日跨ぎ・系列・燃料・上限・
盤面限定の回帰と、Python 59件（実ソルバー連携、ランダム生成、
ローカル探索の有限予算・watchdogを含む）が成功した。
`make test-samples`も公開16×16・24×24・32×32で成功。
本番実行用の`main`を`-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`で再ビルドした。
