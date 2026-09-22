# `auto_prepare.py` の使い方

対象: [`server_client/auto_prepare.py`](../../server_client/auto_prepare.py)

問題を取得して`main`を実行するが、回答システムへのPOSTは一切行わない。接続確認や手動送信前の確認に使う。

## 種別選択

```sh
python3 server_client/auto_prepare.py select
```

`GET /setting`後に次を実行し、`runtime/agents.json`を生成する。

```text
./main select runtime/setting.json runtime/agents.json
```

## 日次探索

```sh
# 1日目（day=0）
python3 server_client/auto_prepare.py day --expected-day 0

# 2日目以降。前日回答に対応するチェックポイントが必要
python3 server_client/auto_prepare.py day --expected-day 1 \
  --score runtime/score-day1.json
```

`runtime/answer.json`と`runtime/score-next.json`を生成する。`score-next.json`はローカル予測なので、対応する回答がサーバーに受理される前に次の日へ使ってはいけない。

## パス指定

- `--solver FILE`: ソルバー実行ファイル。既定は`./main`
- `--work-dir DIR`: JSONとログの保存先。既定は`runtime`
- `--score FILE`: day 1以降に引き継ぐ得点チェックポイント

ソルバーと作業ディレクトリは絶対パスへ解決されるため、`main`が生成する`answer.json`などは指定した作業ディレクトリへ置かれる。
