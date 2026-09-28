# Procon37 競技ソルバー

六角形マップ上で巡回車と補給車を動かす競技問題の回答プログラム。C++17のソルバーがエージェント種別と日次行動を選び、Pythonのビジュアライザーと通信クライアントが回答システムとの取得・送信を担当する。

## 構成

| 場所 | 内容 |
|---|---|
| [`main.cpp`](main.cpp)、[`inc/`](inc/) | 種別選択、経路・行動探索、シミュレーション、公式JSON入出力 |
| [`visualizer/`](visualizer/README.md) | マップ表示、設定取得・種別送信、日次探索、回答送信のGUI |
| [`server_client/`](manual/server_client/README.md) | GUIを使わない取得・送信・全日自動進行 |
| [`tests/`](manual/tests/README.md) | C++・Pythonの回帰テストと計測ツール |
| [`manual/`](manual/README.md) | 実装ファイル別の使い方と詳しい操作手順 |

## GUIで試合を進める

```sh
python3 visualizer/app.py
```

1. 「mainをビルド」を手動で実行する。
2. 「設定取得・種別選択・送信」で試合設定の取得、種別選択、種別送信をまとめて実行する。
3. 対象日の「当日状態を取得・探索」を押す。
4. 日次回答をボタンで送信するか、探索後の自動送信を選ぶ。
5. 回答受理後、対象dayは次の日へ進む。次の日の探索は再びボタンで開始する。

接続先とTOKENは「実行設定・使用ファイル」で指定する。実サーバーへ接続せず試す場合は、同梱の簡易サーバーをGUIから起動できる。マップにはエージェントごとの経路を色分けして表示する。詳細は[ビジュアライザーの説明](visualizer/README.md)を参照する。

## CLIで探索する

```sh
./main select setting.json agents.json
./main solve setting.json state-day0.json
./main solve setting.json state-day1.json score-day1.json
```

`select`は`POST /agent`用の種別配列を、`solve`は`POST /`用の`answer.json`と次日用の`score-next.json`を生成する。`main`自体は送信しない。day 1以降の探索には、前日の回答が受理された後に確定した得点チェックポイントが必要である。通信CLIの使い方は[クライアントの説明](manual/server_client/README.md)を参照する。

`.env`、実行ファイル、生成JSON、`runtime/`はGit管理対象外である。接続用TOKENはGitへ追加しない。
