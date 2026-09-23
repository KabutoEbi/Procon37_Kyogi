# `visualizer_simple_server_test.py`

対象: [`tests/visualizer_simple_server_test.py`](../../tests/visualizer_simple_server_test.py)

同梱の簡易サーバーバイナリをlocalhostで起動し、実C++ソルバーで次を結合テストする。

- `GET /setting`による通信確認と問題取得
- エージェント種別選択と`POST /agent`
- day 0の状態公開待ちと取得
- 日次探索と`POST /action`
- revision、回答アーカイブ、次日チェックポイント
- テスト終了時のサーバープロセス停止

ネットワーク待受けと同梱バイナリを使うため、通常の単体テストではスキップする。リポジトリ直下でソルバーを一時ファイルへビルドし、明示的に有効化する。

```sh
c++ -std=c++17 -O2 -pthread main.cpp -o /tmp/procon37_simple_test
PROCON_TEST_SIMPLE_SERVER=1 \
PROCON_TEST_SOLVER=/tmp/procon37_simple_test \
python3 -m unittest tests/visualizer_simple_server_test.py
```

空いているlocalhostポートを自動選択する。実サーバー用URL・TOKEN・`.env`は使用しない。
