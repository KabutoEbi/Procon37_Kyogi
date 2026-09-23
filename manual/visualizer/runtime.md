# `runtime.py` の使い方

対象: [`visualizer/runtime.py`](../../visualizer/runtime.py)

macOSの古いシステムTkでウィンドウ全体が空白になる問題を避けるため、GUIの初期化前に実行環境を選ぶ。`app.py`から自動的に呼ばれる。

```sh
python3 visualizer/app.py --check-runtime
```

- `supported_tk()`: tkinterが利用可能で、macOSではTk 8.6以降かを確認する。
- `find_runtime()`: 現在のPythonが対応していればそのまま使用する。macOSで非対応の場合、PATH、Homebrewの標準位置、python.orgのFramework内のPythonを検査する。
- `ensure_runtime()`: 必要なら対応Pythonで同じスクリプト・引数を再実行する。
- `describe_runtime()`: Pythonのパス、Pythonバージョン、Tkバージョンを返す。

検査ではウィンドウを作らず、通信や追加インストールも行わない。現在のPythonと同じ実体の候補は除外するため、再起動のループを避ける。対応する実行環境がなければ、必要なTkバージョンをエラー表示する。
