# `auto_submit.py` の使い方

対象: [`server_client/auto_submit.py`](../../server_client/auto_submit.py)

試合設定取得、エージェント種別選択・送信、各日の状態取得・探索・回答送信を順に自動実行する。このスクリプトは実際に回答を送信する。

## 実行

```sh
c++ -std=c++17 -O2 -pthread main.cpp -o main
python3 server_client/auto_submit.py
```

処理順序は次のとおり。

1. `GET /setting`を`runtime/setting.json`へ保存する。
2. `main select`で`agents.json`を生成し、`POST /agent`へ送る。
3. 各日について、期待する`day`の`GET /`を待つ。
4. `main solve`で`answer.json`と`score-next.json`を生成する。
5. 回答を日別に保存し、`POST /`へ送る。
6. 受理後だけ`score-next.json`を次日用チェックポイントへコピーする。

## 生成物

既定の`runtime/`には次が残る。

- `setting.json`, `agents.json`
- `state-dayN.json`, `answer-dayN.json`
- `score-dayN.json`: 受理済み回答に対応する翌日用チェックポイント
- `logs/`: GETレスポンスとPOSTレスポンス

## 主なオプション

```sh
python3 server_client/auto_submit.py \
  --solver ./main \
  --work-dir runtime \
  --wait 3600 \
  --interval 0.5 \
  --timeout 10
```

途中で失敗した場合は終了コード1となる。現状は途中日から自動再開する機能を持たないため、ログ、受理済みrevision、チェックポイントを確認してから手動で続行する。
