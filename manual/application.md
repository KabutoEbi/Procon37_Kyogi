# `application.hpp` の使い方

対象: [`inc/application.hpp`](../inc/application.hpp)

問題生成とローカル探索をコマンドラインから呼ぶためのアプリケーション層である。

## 最小の`main.cpp`

```cpp
#include "application.hpp"

int main(int argc, char* argv[]) {
    procon37::ApplicationConfig defaults;
    defaults.beamConfig.beamWidth = 128;
    return procon37::runApplication(argc, argv, defaults);
}
```

実行例:

```text
./solver create problem.json --width 16 --height 16 --seed 10
./solver solve --supplies 1 --beam-width 128 --joint-beam-width 512
```

`runApplication()`は`create`と`solve`を振り分け、例外をエラー表示と終了コード1へ変換する。

## 補助API

- `makeInitialState()`: 種別列または補給車台数から初日状態を作る
- `makeWaitPlan()`: 当日の全ステップを待機する計画
- `solveDay()`: `brain1`失敗時に待機計画へフォールバック
- `printPlan()`: APIへ送れる二次元整数配列を標準出力
- `writePlanJson()`: `POST /`の本文と同じ二次元配列をJSONファイルへ保存
- `create(argc, argv, defaults)`: createサブコマンド本体
- `runLocalSolver()`: ランダム問題を生成して初日を解く

現在の`runLocalSolver()`は通信せず、ローカル生成問題だけを対象にする。

リポジトリ直下の`main.cpp`は`inc/application.hpp`を相対指定しているため、追加のインクルードパスなしでも実行できる。

```text
c++ -std=c++17 -O2 -pthread main.cpp -o solver
./solver
./solver sample_maps/sample_16.json
```

引数を省略するとランダム問題を生成し、JSONファイルを1つ指定すると公式の`GET /setting`形式として読み込む。現在の`main.cpp`は探索後の計画を再検証し、成功した場合だけ`answer.json`へ保存する。待機は`-1, -14`ではなく`-15`のように連続分を圧縮して出力される。最終経路とステップ追跡はコンソールへ表示せず、探索統計と評価結果だけを確認できる。
