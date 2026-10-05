# `application.hpp` の使い方

対象: [`inc/application.hpp`](../../inc/application.hpp)

問題生成とローカル探索をコマンドラインから呼ぶためのアプリケーション層である。

## 最小の`main.cpp`

```cpp
#include "application.hpp"

int main(int argc, char* argv[]) {
    benzen::ApplicationConfig defaults;
    defaults.beamConfig.beamWidth = 128;
    return benzen::runApplication(argc, argv, defaults);
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
- `solveDay()`: `brain1()`の経路方式比較を含めて探索し、失敗時に待機計画へフォールバック
- `printPlan()`: APIへ送れる二次元整数配列を標準出力
- `writePlanJson()`: `POST /`の本文と同じ二次元配列をJSONファイルへ保存
- `writeAgentKindsJson()`: `POST /agent`の本文と同じ種別配列をJSONファイルへ保存
- `writeScoreCheckpointJson()`: 次の日に引き継ぐ累積得点を保存
- `create(argc, argv, defaults)`: createサブコマンド本体
- `createMatchSetting(argc, argv, defaults)`: `GET /setting`形式のランダム設定を生成
- `runLocalSolver()`: ランダム問題を生成して初日を解く

## 内部構成

ファイル出力は`application_detail::writeJsonFile()`に集約しており、ファイルを開く処理、出力処理、ストリーム状態の確認を同じ手順で行う。`writePlanJson()`、`writeAgentKindsJson()`、`writeScoreCheckpointJson()`、`writeResultStateJson()`は、形式ごとの検証と内容の出力だけを担当する。

`create()`と`createMatchSetting()`に共通する、引数解析、問題生成、保存失敗時の報告、生成結果の表示は`application_detail::createProblem()`が担当する。両APIの違いは、完全な生成問題を保存するか、公式`GET /setting`形式だけを保存するかである。これらは実装詳細用名前空間の関数であり、通常の呼び出し側は公開APIだけを使用する。

公開の生成入口（`create()`、`createMatchSetting()`、`runLocalSolver()`）は[`sample_maps/README.md`](../../sample_maps/README.md)の3区分に適合しない幅・高さ・台数・スポット数・系列数を拒否する。行動回答時間も60秒に固定する。地形生成などの低レベル`ProblemCreator` APIは単体テスト用に任意の小盤面を扱える。

現在の`runLocalSolver()`は通信せず、ローカル生成問題だけを対象にする。

リポジトリ直下の`main.cpp`は`inc/application.hpp`を相対指定しているため、追加のインクルードパスなしでも実行できる。

```text
c++ -std=c++17 -O2 -pthread main.cpp -o solver
./solver
./solver sample_maps/sample_16.json
./solver setting.json state.json score-checkpoint.json
./solver select setting.json agents.json
./solver solve setting.json state.json score-checkpoint.json
./solver generate random-setting.json --width 16 --height 16 --seed 10
```

引数を省略するとランダム問題を生成し、JSONファイルを1つ指定すると公式の`GET /setting`形式として読み込む。2つ目には公式`GET /`の当日状態、3つ目には前日までの得点チェックポイントを指定できる。day 1以降ではチェックポイントが必須で、日番号の不一致を拒否する。

現在の`main.cpp`は`agents.json`、`answer.json`、`score-next.json`、`result-state.json`を生成するが送信はしない。`result-state.json`には探索対象day、探索終了時の`step`、自チームの種別・位置・燃料、スポット残在庫を保存し、ローカル表示に使用する。完了した1日分の計画では`step`は当日のステップ上限と一致する。`score-next.json`は生成計画をローカル実行した予測値なので、通信クライアントは回答が受理された後にだけ次日用チェックポイントとして採用する。探索失敗時は検証済みの全車待機回答へ切り替える。待機は`-1, -14`ではなく`-15`のように連続分を圧縮して出力される。

`select`は種別選択と`agents.json`生成だけで終了する。`solve`は当日状態を必須入力とし、日次探索だけを実行する。`generate`は幅・高さ・台数・スポット数・系列数・日数・1日の秒数・チーム数・seedを受け取り、公式の試合設定形式を保存する。Python通信クライアントとビジュアライザはこれらのモードを呼び分ける。

リポジトリ直下の`main.cpp`では、種別選択を`runSelection()`、日次探索と成果物保存を`runSolver()`へ分離している。出力ファイル名、種別選択失敗の報告、JSON書き出し失敗の報告はそれぞれ共通箇所で管理する。

通信を含む実行方法は[`server_client/README.md`](../server_client/README.md)を参照する。送信なしの確認には`auto_prepare.py`、全日の自動送信には`auto_submit.py`を使用する。

`main.cpp`は[`solver_options.hpp`](solver_options.md)の探索オプションにも対応する。GUIではこの引数を通してビーム幅や時間制限を反映する。`runApplication()`独自のCLIとは引数形式が異なる。
