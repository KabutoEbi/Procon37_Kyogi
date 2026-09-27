# `visualizer_gui_test.py` の使い方

対象: [`tests/visualizer_gui_test.py`](../../tests/visualizer_gui_test.py)

TkでGUIを初期化し、次を検査する。

- 実行設定・使用ファイルの専用ウィンドウの表示と閉じる操作
- TOKENが伏せ字にならず表示・編集できること
- 左端のチェックによる簡易サーバー設定欄の開閉
- 通信確認、探索パラメータ設定、当日状態の取得と探索の連続実行
- 4日分の自動進行、マップとエージェント・スポット表の反映、手動モードへの復帰

デスクトップ接続とTkが必要。

ウィンドウを隠さず表示し、ログが最下部に全幅で、状態表がマップ右側に配置されることも確認する。macOSで古いTkを検出した場合は、アプリと同じ起動処理で対応Pythonへ切り替える。

```sh
python3 tests/visualizer_gui_test.py
```

簡易サーバーの起動結果、HTTP、ソルバー出力はモック化し、実際の`.env`は読み込まない。同梱簡易サーバーと実ソルバーの連携は[`visualizer_simple_server_test.py`](visualizer_simple_server_test.md)を使用する。
