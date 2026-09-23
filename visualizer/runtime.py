"""macOSの古いシステムTkを避けてGUIを起動する。ウィンドウ作成前に実行する。"""

import os
from pathlib import Path
import shutil
import subprocess
import sys


def supported_tk() -> bool:
    try:
        import tkinter
        return sys.platform != "darwin" or tkinter.TkVersion >= 8.6
    except ImportError:
        return False


def candidates():
    # 通常のpython3が/usr/binでも、インストール済みの新しいTkを探す。
    paths = [shutil.which("python3"), "/opt/homebrew/bin/python3", "/usr/local/bin/python3"]
    frameworks = Path("/Library/Frameworks/Python.framework/Versions")
    if frameworks.is_dir():
        paths.extend(str(path) for path in sorted(frameworks.glob("*/bin/python3"), reverse=True))
    seen = {str(Path(sys.executable).resolve())}
    for value in paths:
        if value is None or not Path(value).is_file():
            continue
        path = str(Path(value).resolve())
        if path not in seen:
            seen.add(path)
            yield path


def find_runtime():
    if supported_tk():
        return sys.executable
    if sys.platform == "darwin":
        probe = (
            "import sys, tkinter; "
            "sys.exit(0 if sys.version_info >= (3,9) and tkinter.TkVersion >= 8.6 else 1)"
        )
        for path in candidates():
            try:
                result = subprocess.run(
                    [path, "-c", probe], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, timeout=5, check=False,
                )
            except (OSError, subprocess.TimeoutExpired):
                continue
            if result.returncode == 0:
                return path
    raise RuntimeError(
        "GUIを表示できるTkが見つかりません。macOSではTk 8.6以降が必要です。\n"
        "Tk付きのPythonをインストールし、そのPythonでvisualizer/app.pyを実行してください。\n"
        f"現在のPython: {sys.executable}"
    )


def ensure_runtime():
    selected = find_runtime()
    if Path(selected).resolve() != Path(sys.executable).resolve():
        print(f"GUI用Pythonへ切り替えます: {selected}", file=sys.stderr, flush=True)
        os.execv(selected, [selected, str(Path(sys.argv[0]).resolve()), *sys.argv[1:]])


def describe_runtime():
    import tkinter
    return f"Python: {sys.executable}\nPython version: {sys.version.split()[0]}\nTk: {tkinter.TkVersion}"
