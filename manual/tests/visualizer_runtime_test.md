# `visualizer_runtime_test.py` の使い方

対象: [`tests/visualizer_runtime_test.py`](../../tests/visualizer_runtime_test.py)

GUIを開かず、対応するPythonの維持、古いTkから対応Pythonへの選択、候補がない場合のエラーを検査する。

```sh
python3 -m unittest tests/visualizer_runtime_test.py
```

実機の起動環境は`python3 visualizer/app.py --check-runtime`で確認する。GUIの操作は[`visualizer_gui_test.py`](visualizer_gui_test.md)で検査する。
