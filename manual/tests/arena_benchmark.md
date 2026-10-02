# `arena_benchmark.cpp` の使い方

対象: [`tests/arena_benchmark.cpp`](../../tests/arena_benchmark.cpp)

公開済みの試合詳細APIから取り出した`problem`を、そのまま`MatchSetting`として
全日程探索するための比較ツールである。リポジトリには外部サイトの問題データを
同梱しない。過去試合の`startsAt`はオフライン測定時だけ0へ置き換える。

```sh
make .build/tools/arena_benchmark
.build/tools/arena_benchmark problem.json 1000 8
.build/tools/arena_benchmark problem.json 1000 8 mixed
.build/tools/arena_benchmark problem.json 5000 17 smooth 128 512 0
```

位置引数は順に、問題JSON、1日あたりの探索上限ms、補給車ビットマスク、
任意の交通モード、ビーム幅、同時行動幅、難所翌日配置の0/1、詳細表示の0/1である。
ビット`i`が1ならエージェント`i`を補給車にする。
少なくとも巡回車・補給車を1台ずつ含める。`mixed`は道路セルだけを対象に、
日・位置から再現可能な混雑20%、渋滞10%の条件を生成する。指定しなければ
全道路順調である。

標準出力は日ごとの累積`kinds/daily_kinds/balls`、当日系列ビットマスク、探索時間、展開数と、
最終的な辞書順スコアを出す。アリーナの実戦結果には他チームによる在庫取得と、
直前2日間の道路利用から決まる交通が含まれるため、ローカル値との完全一致ではなく、
系列達成率、日別系列達成率、玉数の水準を比較する。

2026-10-01の公開試合を使った測定値は
[公開アリーナ問題での比較記録](arena-results-2026-10-01.md)を参照。
32x32の追加改善は[難所翌日配置の比較記録](arena-hard-nextday-2026-10-02.md)を参照。
