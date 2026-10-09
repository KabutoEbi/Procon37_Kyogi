# 回答システム用Pythonクライアント

対象: [`server_client/`](../../server_client/)

Python標準ライブラリだけで、問題取得、回答送信、C++ソルバーとの連携を行う。Python 3.9以降を想定する。

## 接続設定

リポジトリ直下にGit管理外の`.env`を作る。

```dotenv
PROCON_URL=http://127.0.0.1:8080
PROCON_TOKEN=replace-with-your-token
```

`.env`はGit管理対象外である。`--env-file path`で別ファイルを指定でき、`--url`と`--token`は環境変数より優先される。トークンがプロセス一覧やシェル履歴へ残り得るため、通常は`--token`より`.env`を使う。

## 4つの入口

- [`get_problem.py`](get_problem.md): 設定または当日状態を取得して保存
- [`send_answer.py`](send_answer.md): 種別または日次回答を送信
- [`auto_prepare.py`](auto_prepare.md): 取得と`main`実行のみ。送信しない
- [`auto_submit.py`](auto_submit.md): 取得、`main`実行、送信を全日自動化

## 事前準備

```sh
c++ -std=c++17 -O2 -pthread main.cpp -o main
python3 server_client/get_problem.py --help
```

Windowsでは`./build-windows.ps1`で`main.exe`を作り、次のように実行する。
`auto_prepare.py`と`auto_submit.py`もWindowsでは`./main.exe`を既定値にする。

```powershell
./build-windows.ps1
py -3 server_client/get_problem.py --help
py -3 server_client/auto_prepare.py select
```

既定では生成物と通信ログを`runtime/`へ置く。どちらもGit管理対象外である。

## 安全な確認順序

最初は送信しない`auto_prepare.py`でJSON取得とソルバー実行を確認する。

```sh
python3 server_client/auto_prepare.py select
python3 server_client/auto_prepare.py day --expected-day 0
```

生成された`runtime/agents.json`や`runtime/answer.json`を確認した後、必要なものだけ`send_answer.py`で送る。本番で全自動化するときだけ`auto_submit.py`を使う。

## 共通動作

- 認証ヘッダーは`Procon-Token`を使う。
- リクエスト間隔は最低0.2秒で、待機ポーリングの既定値は1秒である。3台同時なら合計約3リクエスト/秒になる。
- HTTP 403と接続失敗は、待機取得中に限り再試行する。
- `--timeout`は1リクエストのタイムアウト、`--wait`は取得待ち全体の上限秒数である。
- `--log-dir`または自動実行の`runtime/logs/`には、成功レスポンス、POST本文、本文のあるHTTPエラーレスポンスを時刻付きで保存する。
