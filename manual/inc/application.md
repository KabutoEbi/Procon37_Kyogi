# `application.hpp` の使い方

対象: [`inc/application.hpp`](../../inc/application.hpp)

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
- `writeAgentKindsJson()`: `POST /agent`の本文と同じ種別配列をJSONファイルへ保存
- `writeScoreCheckpointJson()`: 次の日に引き継ぐ累積得点を保存
- `create(argc, argv, defaults)`: createサブコマンド本体
- `runLocalSolver()`: ランダム問題を生成して初日を解く

現在の`runLocalSolver()`は通信せず、ローカル生成問題だけを対象にする。

リポジトリ直下の`main.cpp`は`inc/application.hpp`を相対指定しているため、追加のインクルードパスなしでも実行できる。

```text
c++ -std=c++17 -O2 -pthread main.cpp -o solver
./solver
./solver sample_maps/sample_16.json
./solver setting.json state.json score-checkpoint.json
./solver select setting.json agents.json
./solver solve setting.json state.json score-checkpoint.json
```

引数を省略するとランダム問題を生成し、JSONファイルを1つ指定すると公式の`GET /setting`形式として読み込む。2つ目には公式`GET /`の当日状態、3つ目には前日までの得点チェックポイントを指定できる。day 1以降ではチェックポイントが必須で、日番号の不一致を拒否する。

現在の`main.cpp`は`agents.json`、`answer.json`、`score-next.json`を生成するが送信はしない。`score-next.json`は生成計画をローカル実行した予測値なので、将来通信を追加するときは回答が受理された後にだけ確定値として採用する。探索失敗時は検証済みの全車待機回答へ切り替える。待機は`-1, -14`ではなく`-15`のように連続分を圧縮して出力される。

`select`は種別選択と`agents.json`生成だけで終了する。`solve`は当日状態を必須入力とし、日次探索だけを実行する。Python通信クライアントはこの2モードを呼び分ける。
