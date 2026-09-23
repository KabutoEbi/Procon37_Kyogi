# `backend.py` の使い方

対象: [`visualizer/backend.py`](../../visualizer/backend.py)

GUIから表示処理を除いた、通信、C++ソルバー実行、簡易サーバー管理、成果物検証を担当する。

## 主な型

- `ConnectionConfig`: URL、TOKEN、タイムアウト。`validate()`で必須値を検査する。
- `SearchParameters`: GUIの探索設定。`arguments()`で`main`のCLI引数へ変換する。
- `MapSnapshot`: 設定・当日状態から作る表示専用の不変データ。
- `Workflow`: 手動操作1回分の取得、探索、送信を提供する。
- `AutomaticMatch`: 公開待ちを含む全日自動フローを管理する。
- `SimpleServer`: OS/CPUに合う同梱バイナリの起動、ログ、停止を管理する。

## `Workflow`の主要メソッド

| メソッド | 処理 |
|---|---|
| `check_connection()` | `GET /setting`で到達・認証・形式を確認 |
| `build_solver()` | C++17、`-O2`、`-pthread`で`main.cpp`をビルド |
| `fetch_setting()` / `fetch_state()` | 公式JSONを安全に保存し、dayを検査 |
| `select_agents()` / `solve_day()` | C++の`select` / `solve`モードを実行 |
| `send_agents()` / `send_answer()` | 種別・日次回答をPOST |
| `solve_local()` | 通信せずサンプル設定を探索 |

種別選択後は設定・種別・接続先のハッシュを`selected-agents.json`へ保存する。日次探索後は設定・状態・回答・得点候補・締切・接続先を`prepared-answer.json`へ保存する。送信前に一致を検証し、回答受理後だけ`score-dayN.json`を確定する。

## 簡易サーバー

`simple_server_binary()`はmacOS arm64/amd64、Linux amd64、Windows amd64から現在の環境に合う同梱ファイルを選ぶ。`simple_server_teams()`は設定JSONのチーム名とTOKENを検査して返す。

```python
from pathlib import Path
from backend import DEFAULT_SIMPLE_CONFIG, SimpleServer

server = SimpleServer(Path("runtime/simple-server"))
try:
    info = server.start(DEFAULT_SIMPLE_CONFIG, team_index=0, port=8080)
    print(info.url)
finally:
    server.stop()
```

起動前に使用中ポートを拒否し、起動後はlocalhostへ接続できるまで最大5秒待つ。標準出力と標準エラーは`simple-server.log`へまとめる。通常は直接呼ばず、[`app.py`](app.md)のボタンを使う。

## 自動フローの停止

`AutomaticMatch`は`setting → select → send_agents → state → solve → send_answer`の段階を保持する。`threading.Event`による停止では探索プロセスも終了する。POST直前から応答確認までを自動再開不可として扱い、受理結果が不明な回答を勝手に再送しない。
