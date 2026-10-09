# `send_answer.py` の使い方

対象: [`server_client/send_answer.py`](../../server_client/send_answer.py)

エージェント種別または1日分の行動回答を回答システムへ送信する。このスクリプトは外部状態を変更する。

## 実行例

```sh
# POST /agent
python3 server_client/send_answer.py agent runtime/agents.json

# POST /
python3 server_client/send_answer.py action runtime/answer.json
```

送信前に最低限の形式を検査する。

- `agent`: 空でない整数配列で、各値が巡回車`0`または補給車`1`
- `action`: 空でない二次元整数配列で、各行動が5以下。待機は負数

日次回答では、HTTP 200だけでなくレスポンスの整数`revision`が0以上であることを確認する。負の`revision`は回答拒否として失敗終了する。

`--log-dir DIR`を指定すると、時刻付きファイル名でPOST本文、成功レスポンス、本文のあるHTTPエラーレスポンスを保存する。送信前には対象ファイルと接続先を必ず確認する。
