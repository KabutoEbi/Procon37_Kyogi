# `console.hpp` の使い方

対象: [`inc/console.hpp`](../inc/console.hpp)

盤面、エージェント、探索結果、計画、ステップ追跡、評価内訳を開発用コンソールへ表示する。

## 探索結果をまとめて表示

```cpp
procon37::ConsoleConfig config;
config.showMap = true;
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

個別に`printConsoleMap()`、`printConsoleAgents()`、`printConsolePlan()`、`printConsoleEvaluation()`も呼べる。`printConsoleTrace()`は計画を事前検証し、有効なら再生して`true`、不正なら理由を表示して`false`を返す。

地図記号は`P`が巡回車、`S`が補給車、`U`がスポット、`==`が道路、`^^`が山、`##`が池、`*N`が同じセルの複数台である。
