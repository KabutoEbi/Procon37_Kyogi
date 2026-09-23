# `visualizer_gui_test.py` の使い方

対象: [`tests/visualizer_gui_test.py`](../../tests/visualizer_gui_test.py)

TkでGUIを初期化し、簡易サーバー起動欄、TOKENの直接入力、通信確認、パラメータ設定、4日分の自動進行、マップ反映、手動モードへの復帰を検査する。デスクトップ接続とTkが必要。

ウィンドウを隠さず表示し、マップ部品が画面に配置されていることも確認する。macOSで古いTkを検出した場合は、アプリと同じ起動処理で対応Pythonへ切り替える。

```sh
python3 tests/visualizer_gui_test.py
```

簡易サーバーの起動結果、HTTP、ソルバー出力はモック化し、実際の`.env`は読み込まない。同梱簡易サーバーと実ソルバーの連携は[`visualizer_simple_server_test.py`](visualizer_simple_server_test.md)を使用する。
