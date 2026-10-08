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

途中で失敗した場合は終了コード1となる。停止位置は`automatic-progress.json`へ保存され、GUIと共通のバックエンドで復旧できる。

```sh
python3 server_client/auto_submit.py --work-dir runtime --resume
```

同じ接続先・TOKEN・作業フォルダを指定し、保存ファイルを変更しないこと。POST結果不明の場合、明示再開時だけ同じ日・状態・回答を再送する。日が進んだ場合や締切後は得点を確定せず停止する。受理記録があればPOSTせずチェックポイントを復元する。別の回答への置換は拒否する。種別送信も同じ保存済み候補だけを使用し、開始締切後には再送しない。

`--start-day N --checkpoint score-dayN.json`で指定日から開始することもできる。新規開始は未使用の作業フォルダで行い、再試合もフォルダを分ける。同じフォルダをGUIとCLIから同時に操作しない。

`--time-limit-ms`（既定60000）、`--auto-beam-width`（既定1）、`--send-reserve-ms`（既定5000）、`--clock-margin-ms`（既定1000）を指定できる。通信timeoutの既定は10秒で、60秒条件では保存・送信余裕を含め約47.8秒を日次探索へ使う。種別選択は受付を優先して既定の試走候補を0件、相対上限を10000msとし、全日程の軽量評価後すぐ送信する。自動幅は盤面・当日ステップ数で切り替わり、0で無効化できる。サーバー締切から通信timeout＋時刻余裕＋保存余裕以上を確保する。POSTのtimeoutは残時間以内に短縮し、締切後は送信を開始しない。余裕不足時やソルバーが返らない場合は停止する。時刻同期と実サーバー通信試験は別途必要。

追加保存物: `submission-*.json`（pending／acceptedとrevision）、`submitted-*.json`（不変の回答・種別・得点候補）。これらの削除による強制再開は行わない。
