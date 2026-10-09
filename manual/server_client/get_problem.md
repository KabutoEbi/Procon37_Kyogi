# `get_problem.py` の使い方

対象: [`server_client/get_problem.py`](../../server_client/get_problem.py)

回答システムから試合開始前設定または各日開始時状態を取得し、受信したJSONをそのままファイルへ保存する。

## 実行例

```sh
# GET /setting
python3 server_client/get_problem.py setting runtime/setting.json

# GET / を、day=1になるまで最大1時間待つ
python3 server_client/get_problem.py state runtime/state-day1.json \
  --expected-day 1 --wait 3600 --interval 1
```

`setting`は`GET /setting`、`state`は`GET /`を使用する。出力は一時ファイルを経由して置換されるため、途中までのJSONを完成ファイルとして残さない。

## 主なオプション

- `--expected-day N`: 状態の`day`がNになるまで待つ
- `--wait SECONDS`: 取得待ち上限。既定の0は1回だけ試す
- `--interval SECONDS`: 再試行間隔。既定1秒、0.2秒以上
- `--timeout SECONDS`: 各HTTPリクエストのタイムアウト
- `--log-dir DIR`: 生レスポンスの履歴保存先
- `--env-file FILE`: 接続情報を読むenvファイル

期待日より前の状態は一時ファイルだけに置き、期待日と一致した時点で出力へ置換する。期待日を越えた状態は失敗終了し、既存の出力を上書きしない。取得結果がJSONオブジェクトでない場合や、待機対象以外のHTTPエラーでも失敗終了する。
