# `backend.py` の使い方

対象: [`visualizer/backend.py`](../../visualizer/backend.py)

GUIから表示処理を除いた、通信、C++ソルバー実行、簡易サーバー管理、成果物検証を担当する。

## 主な型

- `ConnectionConfig`: URL、TOKEN、タイムアウト。`validate()`で必須値を検査する。
- `SearchParameters`: GUIの探索設定。`arguments()`で`main`のCLI引数へ変換する。
- `RandomSettingParameters`: ランダム設定の生成条件と値域検査。
- `MapSnapshot`: 設定・当日状態から作る表示専用の不変データ。現在ステップと、各スポットの設定時初期在庫も保持する。
- `SolverResultSummary`: ソルバーのコンソール報告から取り出した探索結果の要約。
- `format_json_for_display()`: 単純な配列・オブジェクトを1行に保ち、入れ子配列を配列単位で改行する表示用整形。
- `trace_answer_routes()`: 当日開始位置と回答の方向値から各車の通過セルを復元する。待機は線に含めない。
- `Workflow`: 手動操作1回分の取得、探索、送信を提供する。
- `AutomaticMatch`: 公開待ちを含む全日自動フローを管理し、GUIの「試合終了まで全自動」から起動するバックエンド。
- `SimpleServer`: OS/CPUに合う同梱バイナリの起動、ログ、停止を管理する。

## `Workflow`の主要メソッド

| メソッド | 処理 |
|---|---|
| `check_connection()` | `GET /setting`で到達・認証・形式を確認 |
| `build_solver()` | C++17、`-O2`、`-pthread`で`main.cpp`をビルド |
| `generate_random_setting()` | C++の`generate`モードで公式設定形式を一時生成し、検査後に保存 |
| `fetch_setting()` / `fetch_state()` | 公式JSONを安全に保存し、dayを検査 |
| `select_agents()` / `solve_day()` | C++の`select` / `solve`モードを実行 |
| `send_agents()` / `send_answer()` | 種別・日次回答をPOST |
| `solve_local()` | 通信せずサンプル設定を探索し、`result-state.json`の生成を確認 |

種別選択後は設定・種別・接続先のハッシュを`selected-agents.json`へ保存する。日次探索後は設定・状態・回答・得点候補・締切・接続先を`prepared-answer.json`へ保存する。送信前に一致を検証し、回答受理後だけ`score-dayN.json`を確定する。

## 共通検証とCLI引数

`SearchParameters.arguments()`と`RandomSettingParameters.arguments()`は、それぞれの値域を検証した後、共通の`_option_arguments()`でCLI引数へ変換する。Python内部の`snake_case`はC++側の`--kebab-case`へ変換され、送信専用の余裕設定やローカル交通設定は必要な呼び出し先へだけ渡す。

日次探索とローカル探索は`_day_duration()`で`daySeconds`の存在、対象日、数値型を共通検証する。ソルバーを起動する操作は`Workflow._require_solver()`で実行ファイルの存在を同じエラー形式で確認する。これらの補助関数は内部用で、`Workflow`の公開操作手順は変わらない。

`day_time_limit_ms()`は対象日の`daySeconds`をミリ秒へ変換し、GUIが設定読込時とday変更時に探索時間上限の初期値として使用する。値は実行前に短く変更でき、バックエンドでも対象日の試合時間を超えないよう制限する。

`RandomSettingParameters`の既定値は32×32、8台、32スポット、20系列、10日である。`SearchParameters.time_limit_ms`の設定未読込時の既定値は60000msだが、GUIでは有効な設定を読み込むと対象日の`daySeconds`へ置き換える。

## 同じ日の再探索・回答更新

同じ日・設定・接続先・開始状態の受理済み回答は、新しい探索結果で更新できる。新候補は別名のスナップショットへ保存してから送信記録を切り替え、旧受理記録を履歴へ残す。新候補が受理されるまでは既存の次日得点を維持する。結果不明の候補や異なる試合の記録は置換しない。同一の受理済み候補は重複送信しない。

更新時は`submitted-dayN-<候補ハッシュ>.json`と対応する得点ファイルを保存し、最後に`submission-dayN.json`を切り替える。前の受理記録は`submission-dayN-accepted-<revision>.json`へ保存する。従来の記録にはスナップショット接尾辞がないため、従来のファイル名を使用して復旧する。

同日の再探索に渡すチェックポイントは`score-dayN.json`であり、送信後に作る`score-dayN+1.json`ではない。更新送信の結果が不明な間は、`solve_day()`が同日・翌日の探索を拒否する。旧チェックポイントが残っていても、それを確定した最新得点として次日へ引き継がない。

対象日の終了時刻監視と取得状態の再利用はGUI側の処理である。`AutomaticMatch`の全日自動フローとは分けて扱う。

## 結果の表示

`load_snapshot()`は通常の当日状態に加え、探索結果の`step`と`remainingStocks`を読める。`step`がない通常の当日状態は開始時点の0として扱う。設定側の初期在庫は`stock_limits`へ保持するため、ローカル探索後のエージェント位置・燃料・現在ステップとスポットの残数/初期数を同じ状態表示へ反映できる。マップ上部のスポット数は`spots`の件数、系列数は`spots`に含まれる系列IDの重複なし件数から求める。
`parse_solver_result()`はコンソール報告からday、終了理由、探索時間、展開数、その日の総獲得数、全日程の合計獲得数、当日・全日程の獲得系列数を読み取り、GUIの探索結果枠へ渡す。報告が欠けている場合は古い結果を流用しない。
`daily_acquisition_theoretical()`は移動距離を無視し、巡回車数、スポット在庫、巡回車ごとの同一スポット1日1回制約、当日のステップ数から、当日の獲得数と系列数の理論上限を返す。
`match_acquisition_theoretical()`はこの日別理論上限を全日程について合算する。GUIの当日表示は設定時点の単日上限を使い、全日程の合計表示は日別理論上限の合計を使う。全日程の系列実績はC++の`total-daily-brands`（各日の取得系列数の累計）を表示し、`match-brands`（試合全体の重複なし系列数）とは区別する。

`format_json_for_display()`は画面表示用の文字列だけを整形する。単純な配列・オブジェクトは1行、入れ子の配列・オブジェクトは項目単位で改行し、整形後も同じJSON値へ再パースできる。取得ファイル、回答ファイル、送信本文は書き換えない。

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

## バックエンドの全日自動フロー

`AutomaticMatch`は`setting → select → send_agents → state → solve → send_answer`を`automatic-progress.json`へ原子的に保存する。`restore()`は接続先・TOKENのハッシュと使用ファイルのハッシュを検査して復旧する。`threading.Event`による停止では探索プロセスも終了する。

POST前に`submission-agents.json`／`submission-dayN.json`と不変の候補`submitted-*.json`を保存する。受理記録を先に保存し、その後にチェックポイントを確定するため、途中終了しても受理済み候補から復元できる。結果不明の別回答への置換を拒否し、明示再開時だけ当日状態を再取得して同一回答を再送する。締切後や日程進行後は未確定のまま停止する。APIには最後の有効回答を照会する機能がないため、その場合の自動確定は行わない。

`SimpleServer.clear_client_work()`は簡易サーバー停止中に限り、`runtime/simple-server/client/`直下のクライアント成果物を削除して空のフォルダを残す。サーバーログと設定JSON原本は対象外である。GUIでは確認ダイアログを通して呼び出す。

サーバー締切がある場合、`max(safety_margin_ms, send_reserve_ms, timeout*1000 + clock_margin_ms + 1200)`を探索から確保する。相対探索予算を保つためCLIへ渡す相対上限を補正する。プロセス監視とPOST直前・レート制限待機後の締切検査も行う。時刻差は指定余裕で吸収する方式で、自動同期や受理保証ではない。同じ作業フォルダの同時使用は禁止。

## ランダム生成時の道路状態

`RandomSettingParameters.include_congestion`は0/1で指定し、既定は0。C++の生成引数へは渡さず、0なら全道路を順調、1なら地形生成後にseed付きで順調・混雑・渋滞を道路セルへ割り当て、`localTraffics`へ保存する。

`solve_local()`は`localTraffics`がある設定について、種別選択後に開始位置・満タン燃料・選択交通から`local-state.json`を作り、その状態で探索する。探索後の`result-state.json`にも交通を保持する。`load_snapshot()`は初期表示に設定の`localTraffics`を使い、当日状態がある場合はその`traffics`を優先する。
