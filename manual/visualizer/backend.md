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
| `run_dummy_connection_test()` | 設定公開を最大25秒待ち、実サーバーから問題・4日分の状態を取得して、ダミー種別と全ステップ待機だけを送信。20秒×4日の設定を送信前に検査 |
| `build_solver()` | C++17、`-O3 -DNDEBUG`、厳格警告、`-pthread`で担当共通の`main.cpp`を本番ビルド |
| `generate_random_setting()` | C++の`generate`モードで公式設定形式を一時生成し、検査後に保存 |
| `fetch_setting()` / `fetch_state()` | 公式JSONを安全に保存し、dayを検査 |
| `wait_for_setting()` | 未公開中の403・一時的な接続失敗を1秒間隔で再試行し、停止要求へ応答 |
| `wait_for_state()` | 未公開の当日状態を待機し、前日の応答は再試行、期待日を越えた応答は拒否 |
| `select_agents()` / `solve_day()` | C++の`select` / `solve`モードを実行 |
| `send_agents()` / `send_answer()` | 種別・日次回答をPOST |
| `import_spoken_checkpoint()` | 口頭で受けた3整数へ対象day由来の`nextDay`を加え、次日チェックポイントとして保存 |
| `publish_checkpoint()` / `import_checkpoint()` | 旧共有フォルダ方式の互換用API |
| `solve_local()` | 通信せずサンプル設定を探索し、`result-state.json`の生成を確認 |

設定取得時刻と設定ハッシュを`setting-received.json`へ保存し、盤面幅に対応する公式の種別選択時間（60／90／120秒）から受付終了を求める。同じ設定を再取得しても時刻を延長しない。`startsAt`がそれより早い場合だけ上限として使う。種別選択は試走なしの初期構成を約5秒で提出してから、初期構成を現行ベストとして6候補を受付終了10秒前まで比較する。厳密に改善した構成だけ2回目を送信する。日次回答は、貪欲解の即時提出、ビームまたはSAの停滞時の改善提出、当日終了10秒前の最終改善提出という3段階である。中間・最終候補は公式辞書順得点が提出済みベストより高い場合だけ送信する。日次探索後は設定・状態・回答・得点候補・締切・接続先を`prepared-answer.json`へ保存する。送信前に一致を検証し、回答受理後だけ`score-dayN.json`を確定する。

通常の接続timeoutは10秒。SSLハンドシェイクを含む接続timeoutでPOST結果が不明になった場合、全自動は自動再送せず停止する。エラーには`timeout(s)`を15へ増やし、作業フォルダを維持したまま再開する手順を表示する。再開処理は保存済みハッシュを検査し、同じ種別・回答だけを扱う。

## 共通検証とCLI引数

`SearchParameters.arguments()`と`RandomSettingParameters.arguments()`は、それぞれの値域を検証した後、共通の`_option_arguments()`でCLI引数へ変換する。Python内部の`snake_case`はC++側の`--kebab-case`へ変換され、`udon_sa`は`--udon-sa`として渡る。送信専用の余裕設定やローカル交通設定は必要な呼び出し先へだけ渡す。

日次探索とローカル探索は`_day_duration()`で`daySeconds`の存在、対象日、数値型を共通検証する。ソルバーを起動する操作は`Workflow._require_solver()`で実行ファイルの存在を同じエラー形式で確認する。これらの補助関数は内部用で、`Workflow`の公開操作手順は変わらない。

`day_time_limit_ms()`は対象日の`daySeconds`をミリ秒へ変換し、GUIが設定読込時とday変更時に探索時間上限の初期値として使用する。値は実行前に短く変更でき、バックエンドでも対象日の試合時間を超えないよう制限する。

ローカル探索では種別選択と日次探索を別プロセスで実行する。`selection_time_ms=0`の既定は種別選択上限10秒へ変換し、日次予算が短ければその値へ短縮する。明示した種別選択時間も盤面別受付枠以内へ制限する。ローカル種別選択と日次探索は、C++の時間制限に加えてPython側も上限+2秒でプロセスを終了させる。従来は生成設定の`localTraffics`がある場合の`select`へ0を明示して無制限試走になっていた。通常の設定ファイルにも同じ有限選択を適用し、交通指定がない道路は順調として初期状態を作る。初期値では種別選択と日次探索で最大約70秒を要する。

`RandomSettingParameters`の既定値は16×16、4台、16スポット、14系列、10日である。生成できる寸法・台数・スポット数・系列数の組合せは[`sample_maps/README.md`](../../sample_maps/README.md)の3区分のみで、`seconds`は60固定。条件外はソルバー起動前に`WorkflowError`で拒否し、C++の生成入口でも同じ条件を検査する。`SearchParameters.time_limit_ms`の設定未読込時の既定値は60000msだが、GUIでは有効な設定を読み込むと対象日の`daySeconds`へ置き換える。

## 同じ日の再探索・回答更新

同じ日・設定・接続先・開始状態の受理済み回答は、公式第1〜3基準が辞書順で高い新しい探索結果だけで更新できる。同点または悪化候補は、最後の有効回答時刻という第4基準を悪化させるためPOST前に拒否する。新候補は別名のスナップショットへ保存してから送信記録を切り替え、旧受理記録を履歴へ残す。新候補が受理されるまでは既存の次日得点を維持する。結果不明の候補や異なる試合の記録は置換しない。同一の受理済み候補は通常は重複送信しないが、待機中に他端末との比較でベストと確認した場合は、GUIから`force=True`の明示再送を行える。この明示再送では第4基準が悪化する旨を確認画面へ表示する。

更新時は`submitted-dayN-<候補ハッシュ>.json`と対応する得点ファイルを保存し、最後に`submission-dayN.json`を切り替える。前の受理記録は`submission-dayN-accepted-<revision>.json`へ保存する。従来の記録にはスナップショット接尾辞がないため、従来のファイル名を使用して復旧する。

同日の再探索に渡すチェックポイントは`score-dayN.json`であり、送信後に作る`score-dayN+1.json`ではない。更新送信の結果が不明な間は、`solve_day()`が同日・翌日の探索を拒否する。旧チェックポイントが残っていても、それを確定した最新得点として次日へ引き継がない。

対象日の終了時刻監視と取得状態の再利用はGUI側の処理である。`AutomaticMatch`の全日自動フローとは分けて扱う。

`Workflow.artifact_lock`は、待機中の明示再送と次日探索が同じ`answer.json`や得点ファイルを同時更新しないよう直列化する。3人運用の翌日先行取得と`AutomaticMatch`の状態公開待ちは、どちらも`wait_for_state()`のday検査を使用する。

## 結果の表示

`load_snapshot()`は通常の当日状態に加え、探索結果の`step`と`remainingStocks`を読める。`step`がない通常の当日状態は開始時点の0として扱う。設定側の初期在庫は`stock_limits`へ保持するため、ローカル探索後のエージェント位置・燃料・現在ステップとスポットの残数/初期数を同じ状態表示へ反映できる。マップ上部のスポット数は`spots`の件数、系列数は`spots`に含まれる系列IDの重複なし件数から求める。
`parse_solver_result()`はコンソール報告からday、終了理由、探索時間、展開数、その日の総獲得数、全日程の合計獲得数、当日・全日程の獲得系列数を読み取り、GUIの探索結果枠へ渡す。GUIは`match-brands / total-daily-brands / total-udon`を公式辞書順の比較値として併記する。報告が欠けている場合は古い結果を流用しない。
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

通常モードと担当1は全自動を最終日まで利用できる。担当1は問題公開待ちから種別選択・送信、全日の探索・回答送信までを実行する。担当2・3だけGUI側で全自動起動を無効化し、手動送信を個別操作として実行する。担当2・3の次日状態先行取得は全自動とは独立したバックグラウンド処理である。

現在のGUIは口頭引継ぎを使用する。`parse_spoken_checkpoint()`は空白区切りの`系列mask 玉数 日別系列累計`を3個の10進整数として検査し、GUIで選んだ対象dayから得た`nextDay`を付ける。`Workflow.import_spoken_checkpoint()`はその4項目を`score-dayN.json`へ原子的に保存する。day 1相当の`nextDay=0`は、直前日のチェックポイントが存在しないため拒否する。GUIは保存成功後だけ入力欄を空にし、検査エラー時は修正できるよう入力を残す。最終回答受理後または対象dayが総日数を越えた状態では、バックエンドを呼ばずに取込不要と案内して新しいチェックポイントを作らない。

`Workflow.export_checkpoint_code()`と`import_checkpoint_code()`は旧方式との互換用に残るが、現在のGUIには表示しない。前者は設定hash・接続先とTOKENから作るチーム識別hash・`nextDay`・revision・得点チェックポイントを署名付き`P37C1:`コードにし、後者は検証後に取り込む。

`Workflow.shared_dir`を使う旧共有フォルダ処理も互換用に残るが、現在のGUIでは共有フォルダを設定しない。

POST前に`submission-agents.json`／`submission-dayN.json`と不変の候補`submitted-*.json`を保存する。受理記録を先に保存し、その後にチェックポイントを確定するため、途中終了しても受理済み候補から復元できる。結果不明の別回答への置換を拒否し、明示再開時だけ当日状態を再取得して同一回答を再送する。締切後や日程進行後は未確定のまま停止する。APIには最後の有効回答を照会する機能がないため、その場合の自動確定は行わない。

`SimpleServer.clear_client_work()`は簡易サーバー停止中に限り、`runtime/simple-server/client/`直下のクライアント成果物を削除して空のフォルダを残す。サーバーログと設定JSON原本は対象外である。GUIでは確認ダイアログを通して呼び出す。

自動送信の日次処理は、まず`greedy`モードの最短路ベース解を即時提出する。続く停滞探索と最終探索は`endsAt`と`daySeconds`から公式回答枠を使い、10,000msを送信余裕として差し引く。停滞探索では連続するビーム幅の公式得点、またはSA近傍の改善停滞を検査して早期終了する。各提出のHTTP受理後に絶対締切の10秒前まで残時間があれば次段階を開始する。POST直前とレート制限待機後にも締切を検査し、残時間に合わせて通信timeoutを短縮する。手動送信する担当2・3は従来の時間プリセットと5秒安全余裕を使う。同じ作業フォルダの同時使用は禁止。待機ポーリングの既定は1秒である。

通信監査ではGET／POSTの成功レスポンスに加え、POST本文と本文のあるHTTPエラーレスポンスを`logs/`へ時刻付きで保存する。送信記録には作成・試行・受理時刻を残す。

## ランダム生成時の道路状態

`RandomSettingParameters.include_congestion`は0/1で指定し、既定は0。C++の生成引数へは渡さず、0なら全道路を順調、1なら地形生成後に順調・混雑・渋滞を道路セルへ割り当て、`localTraffics`へ保存する。`Workflow.generate_random_setting()`は呼出しごとに新しい64bit seedを選び、パラメータの`seed`値より優先する。地形と道路状態は同じ生成seedを使い、実行コマンドとログからそのseedを確認できる。

`solve_local()`は`localTraffics`がある設定について、種別選択後に開始位置・満タン燃料・選択交通から`local-state.json`を作り、その状態で探索する。探索後の`result-state.json`にも交通を保持する。`load_snapshot()`は初期表示に設定の`localTraffics`を使い、当日状態がある場合はその`traffics`を優先する。
