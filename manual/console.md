# `console.hpp` の使い方

対象: [`inc/console.hpp`](../inc/console.hpp)

盤面、エージェント、探索結果、計画、ステップ追跡、評価内訳を開発用コンソールへ表示する。

## 探索結果をまとめて表示

```cpp
procon37::ConsoleConfig config;
config.showMap = true;
config.showFinalMap = false;
config.showPlan = true;
config.showEvaluation = true;
config.showTrace = true;
config.traceEveryStep = false;
config.maxTraceLines = 120;

procon37::printConsoleReport(
    std::cout,
    setting,
    matchState,
    searchResult,
    previousScore,
    config);
```

`traceEveryStep=false`では、移動・燃料・取得などに変化があったステップと最終ステップを表示する。`maxTraceLines`を超えた部分は省略される。

`showMap`は開始時マップ、`showFinalMap`は結果の下に再表示する終了時マップを制御する。終了時マップは既定で非表示である。

個別に`printConsoleMap()`、`printConsoleAgents()`、`printConsolePlan()`、`printConsoleEvaluation()`も呼べる。`printConsoleTrace()`は計画を事前検証し、有効なら再生して`true`、不正なら理由を表示して`false`を返す。

試合条件、マップ、エージェント、探索統計、評価値、最終結果は空行で区切られ、統計と評価の各項目は1行ずつ表示される。

地図は`0`が平地、`1`が順調な道路、`2`が山、`3`が池、`4`が混雑道路、`5`が渋滞道路である。通常セルは角括弧なしで表示する。スポットだけは`[系列番号]`と表示する。同じ系列に属する複数のスポットは同じ番号になる。エージェントの位置と種別は地図直下の一覧で確認する。

エージェントは`[ID]S/P [pos]セル番号(x,y) [fuel]現在値/上限`の形式で表示する。例えば巡回車0は`[0]P [pos]5(5,0) [fuel]40/40`となる。

セル間には1文字の空白だけを置く。
寸法と記号の凡例はコンソールへ表示せず、マップ本体だけを出力する。
