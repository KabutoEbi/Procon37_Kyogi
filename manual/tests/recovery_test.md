# 復旧・締切管理テスト

```sh
python3 -m unittest tests/recovery_test.py
```

外部通信なしで、応答喪失後のプロセス再起動と同一回答再送、受理記録保存後のチェックポイント書込み失敗、未知結果の得点確定禁止、新しい探索の禁止、接続先・ファイル変更検出、締切切れ、レート制限待機後の締切検査、相対探索予算維持、ソルバー監視を検証する。

実C++ソルバーでの4日間連携は`PROCON_TEST_SOLVER`を指定した`tests/visualizer_test.py`、同梱簡易サーバーの実通信は`tests/visualizer_simple_server_test.py`で確認する。実サーバーの受理・時刻差を保証するテストではない。
