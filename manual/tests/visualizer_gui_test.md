# `visualizer_gui_test.py` の使い方

対象: [`tests/visualizer_gui_test.py`](../../tests/visualizer_gui_test.py)

TkでGUIを初期化し、次を検査する。

- 実行設定・使用ファイルの専用ウィンドウの表示と閉じる操作
- TOKENが伏せ字にならず表示・編集できること
- 左端のチェックによる簡易サーバー設定欄の開閉
- 通信確認、探索パラメータ設定、設定取得・種別選択・送信の一括実行
- 当日状態の取得と探索、手動送信、探索後の自動送信と次の日の手動開始
- マップ上のエージェント別経路色、次の探索開始時の線の消去
- 対象dayの自動更新と設定日数に合わせた入力範囲
- マップとエージェント・スポット表の反映

デスクトップ接続とTkが必要。

ウィンドウを隠さず表示し、ログが最下部に全幅で、状態表がマップ右側に配置されることも確認する。macOSで古いTkを検出した場合は、アプリと同じ起動処理で対応Pythonへ切り替える。

```sh
python3 tests/visualizer_gui_test.py
```

簡易サーバーの起動結果、HTTP、ソルバー出力はモック化し、実際の`.env`は読み込まない。同梱簡易サーバーと実ソルバーの連携は[`visualizer_simple_server_test.py`](visualizer_simple_server_test.md)を使用する。
