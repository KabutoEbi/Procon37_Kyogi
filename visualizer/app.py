#!/usr/bin/env python3
"""Procon37の取得・探索・送信を操作するTk GUI。"""

from __future__ import annotations

import sys
from runtime import describe_runtime, ensure_runtime, supported_tk

# Tkや画面部品を初期化する前に実行環境を選ぶ。
if __name__ == "__main__":
    try:
        ensure_runtime()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
    if "--check-runtime" in sys.argv:
        print(describe_runtime())
        raise SystemExit(0)

import json
import math
import queue
import threading
from dataclasses import replace
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText
from typing import Any, Callable

from backend import (
    PROJECT_ROOT,
    CommandResult,
    ConnectionConfig,
    MapSnapshot,
    Workflow,
    WorkflowError,
    AutomaticMatch,
    SearchParameters,
    SimpleServer,
    SimpleServerInfo,
    DEFAULT_SIMPLE_CONFIG,
    Paused,
    connection_from_env,
    load_snapshot,
    read_json,
    simple_server_teams,
)


class SolverGUI(tk.Tk):
    TERRAIN_COLORS = {
        0: "#e9e4d4",
        1: "#cfd5dc",
        2: "#a98c6b",
        3: "#7fc4de",
    }
    TRAFFIC_COLORS = {1: "#f2b84b", 2: "#df6657"}

    def __init__(self) -> None:
        if not supported_tk():
            raise RuntimeError("このPythonのTkはGUI表示に非対応です。python3 visualizer/app.pyで起動してください")
        super().__init__()
        self.title("Procon37 Control Center")
        self.geometry("1380x880")
        self.minsize(1040, 700)

        self._messages: queue.Queue[tuple[str, Any, Any]] = queue.Queue()
        self._busy = False
        self._action_buttons: list[ttk.Button] = []
        self._manual_buttons: list[ttk.Button] = []
        self._current_snapshot: MapSnapshot | None = None
        self._stop = threading.Event()
        self._automatic: AutomaticMatch | None = None
        self._closing = False
        self._active_token = ""
        self._selected_kinds: list[int] = []
        self._simple_server = SimpleServer(PROJECT_ROOT / "runtime/simple-server")
        self._simple_teams: tuple[tuple[str, str], ...] = ()
        self.mode_var = tk.StringVar(value="manual")
        self.scope_var = tk.StringVar(value="試合開始前から")
        self.interval_var = tk.StringVar(value="0.5")
        self.wait_var = tk.StringVar(value="3600")
        self.parameter_vars = {
            name: tk.StringVar(value=str(value))
            for name, value in vars(SearchParameters()).items()
        }

        self.env_var = tk.StringVar(value=str(PROJECT_ROOT / ".env"))
        self.url_var = tk.StringVar()
        self.token_var = tk.StringVar()
        self.timeout_var = tk.StringVar(value="10")
        self.solver_var = tk.StringVar(value=str(PROJECT_ROOT / "main"))
        self.work_var = tk.StringVar(value=str(PROJECT_ROOT / "runtime"))
        self.setting_var = tk.StringVar(value=str(PROJECT_ROOT / "runtime/setting.json"))
        self.state_var = tk.StringVar(value=str(PROJECT_ROOT / "runtime/state-day0.json"))
        self.score_var = tk.StringVar()
        self.day_var = tk.IntVar(value=0)
        self.status_var = tk.StringVar(value="準備完了")
        self.map_info_var = tk.StringVar(value="試合設定を取得または読み込んでください")
        self.simple_config_var = tk.StringVar(value=str(DEFAULT_SIMPLE_CONFIG))
        self.simple_team_var = tk.StringVar()
        self.simple_port_var = tk.StringVar(value="8080")
        self.simple_kind_deadline_var = tk.StringVar(value="30")
        self.simple_start_delay_var = tk.StringVar(value="2")

        self._configure_style()
        self._build_ui()
        self._reload_simple_teams(show_dialog=False)
        self._reload_env(show_dialog=False)
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.after(100, self._poll_messages)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("Title.TLabel", font=("TkDefaultFont", 17, "bold"))
        style.configure("Section.TLabel", font=("TkDefaultFont", 11, "bold"))
        style.configure("Status.TLabel", padding=(8, 5))
        style.configure("Primary.TButton", font=("TkDefaultFont", 10, "bold"))

    def _build_ui(self) -> None:
        header = ttk.Frame(self, padding=(12, 10, 12, 6))
        header.pack(fill=tk.X)
        ttk.Label(header, text="Procon37 Control Center", style="Title.TLabel").pack(side=tk.LEFT)
        ttk.Label(
            header,
            text="取得 → 種別選択 → 状態取得 → 探索 → 送信",
            foreground="#59636e",
        ).pack(side=tk.LEFT, padx=18)

        config = ttk.LabelFrame(self, text="実行設定", padding=8)
        self.config_frame = config
        config.pack(fill=tk.X, padx=12, pady=(0, 8))
        for column in (1, 4, 5):
            config.columnconfigure(column, weight=1)

        self._path_row(config, 0, 0, ".env", self.env_var, self._choose_env)
        ttk.Button(config, text="再読込", command=self._reload_env).grid(
            row=0, column=3, padx=(5, 14), sticky=tk.W
        )
        ttk.Label(config, text="URL").grid(row=0, column=4, sticky=tk.E, padx=(0, 5))
        ttk.Entry(config, textvariable=self.url_var).grid(row=0, column=5, sticky=tk.EW)

        self._path_row(config, 1, 0, "solver", self.solver_var, self._choose_solver)
        self._path_row(config, 1, 3, "work", self.work_var, self._choose_work_dir)

        ttk.Label(config, text="TOKEN").grid(row=2, column=0, sticky=tk.E, padx=(0, 5))
        ttk.Entry(config, textvariable=self.token_var, show="●").grid(
            row=2, column=1, columnspan=2, sticky=tk.EW
        )
        ttk.Label(config, text="timeout(s)").grid(row=2, column=3, sticky=tk.E, padx=(12, 5))
        ttk.Entry(config, textvariable=self.timeout_var, width=8).grid(row=2, column=4, sticky=tk.W)
        check = ttk.Button(config, text="通信確認（手動）", command=self._check_connection)
        check.grid(row=2, column=5, sticky=tk.E)
        self._action_buttons.append(check)

        simple = ttk.LabelFrame(self, text="同梱の簡易サーバー", padding=8)
        self.simple_frame = simple
        simple.pack(fill=tk.X, padx=12, pady=(0, 8))
        simple.columnconfigure(1, weight=1)
        ttk.Label(simple, text="設定JSON").grid(row=0, column=0, sticky=tk.E, padx=(0, 5))
        ttk.Entry(simple, textvariable=self.simple_config_var).grid(
            row=0, column=1, sticky=tk.EW
        )
        ttk.Button(simple, text="選択", command=self._choose_simple_config).grid(
            row=0, column=2, padx=(5, 12)
        )
        ttk.Label(simple, text="チーム").grid(row=0, column=3, sticky=tk.E)
        self.simple_team_box = ttk.Combobox(
            simple, textvariable=self.simple_team_var, width=14, state="readonly"
        )
        self.simple_team_box.grid(row=0, column=4, padx=(5, 12))
        ttk.Label(simple, text="port").grid(row=0, column=5, sticky=tk.E)
        ttk.Entry(simple, textvariable=self.simple_port_var, width=7).grid(
            row=0, column=6, padx=(5, 12)
        )
        ttk.Label(simple, text="種別締切(s)").grid(row=0, column=7, sticky=tk.E)
        ttk.Entry(simple, textvariable=self.simple_kind_deadline_var, width=7).grid(
            row=0, column=8, padx=(5, 12)
        )
        ttk.Label(simple, text="開始待ち(s)").grid(row=0, column=9, sticky=tk.E)
        ttk.Entry(simple, textvariable=self.simple_start_delay_var, width=7).grid(
            row=0, column=10, padx=(5, 12)
        )
        self.simple_start_button = ttk.Button(
            simple, text="起動して使用", command=self._start_simple_server,
            style="Primary.TButton",
        )
        self.simple_start_button.grid(row=0, column=11, padx=(0, 5))
        self.simple_stop_button = ttk.Button(
            simple, text="停止", command=self._stop_simple_server, state=tk.DISABLED
        )
        self.simple_stop_button.grid(row=0, column=12)
        self._action_buttons.append(self.simple_start_button)

        mode = ttk.Frame(self, padding=(12, 0, 12, 8))
        mode.pack(fill=tk.X)
        for value, label in (("manual", "手動"), ("auto", "自動")):
            ttk.Radiobutton(mode, text=label, variable=self.mode_var, value=value,
                            command=lambda: self._mode_changed(True)).pack(side=tk.LEFT, padx=4)
        self.scope_box = ttk.Combobox(
            mode, textvariable=self.scope_var, width=20, state="readonly",
            values=("試合開始前から", "対象dayから（種別送信済）", "停止位置から再開"),
        )
        self.scope_box.pack(side=tk.LEFT, padx=10)
        self.auto_button = ttk.Button(mode, text="自動開始（送信あり）", command=self._start_auto)
        self.auto_button.pack(side=tk.LEFT)
        self.stop_button = ttk.Button(mode, text="停止", command=self._request_stop, state=tk.DISABLED)
        self.stop_button.pack(side=tk.LEFT, padx=6)
        ttk.Label(mode, text="dayは0始まり / 設定変更は次の実行から適用").pack(side=tk.LEFT, padx=10)

        vertical = ttk.Panedwindow(self, orient=tk.VERTICAL)
        vertical.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 8))
        main = ttk.Panedwindow(vertical, orient=tk.HORIZONTAL)
        vertical.add(main, weight=4)

        control_panel, controls = self._scroll_panel(main, 330)
        self.controls_frame = controls
        main.add(control_panel, weight=0)
        self._build_workflow(controls)

        notebook = ttk.Notebook(main)
        main.add(notebook, weight=1)
        self._build_map_tab(notebook)
        self._build_json_tab(notebook)
        self._build_parameters_tab(notebook)

        log_frame = ttk.LabelFrame(vertical, text="実行ログ", padding=5)
        vertical.add(log_frame, weight=1)
        self.log_text = ScrolledText(log_frame, height=9, wrap=tk.WORD, state=tk.DISABLED)
        self.log_text.pack(fill=tk.BOTH, expand=True)

        status = ttk.Label(self, textvariable=self.status_var, style="Status.TLabel", anchor=tk.W)
        status.pack(fill=tk.X, padx=8, pady=(0, 5))
        self._mode_changed()

    def _build_parameters_tab(self, notebook: ttk.Notebook) -> None:
        panel, frame = self._scroll_panel(notebook, 650)
        notebook.add(panel, text="探索パラメータ")
        self.parameters_frame = frame
        fields = [
            ("beam_width", "ビーム幅", "各展開で保持する状態数"),
            ("joint_beam_width", "同時行動ビーム幅", "複数台への行動割当の幅"),
            ("threads", "スレッド数", "0: 自動 / 1: 直列"),
            ("time_limit_ms", "探索時間上限 (ms)", "0: 相対制限なし。サーバー締切は常に有効"),
            ("safety_margin_ms", "締切余裕 (ms)", "探索・種別選択を締切より早く終了"),
            ("max_actions", "各車の行動候補数", "0: 全候補"),
            ("macro_actions", "マクロ行動", "1: 有効 / 0: 無効"),
            ("adaptive_beam", "ビーム幅自動調整", "1: 有効 / 0: 指定幅を使用"),
            ("trial_candidates", "種別選択の試走候補数", "0: 距離評価のみ"),
            ("trial_beam_width", "種別選択の試走ビーム幅", "1以上"),
            ("trial_joint_beam_width", "種別選択の同時行動幅", "1以上"),
            ("selection_time_ms", "種別選択時間上限 (ms)", "0: 相対制限なし"),
        ]
        for row, (key, label, hint) in enumerate(fields):
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky=tk.W, pady=4)
            ttk.Entry(frame, textvariable=self.parameter_vars[key], width=12).grid(
                row=row, column=1, padx=12, pady=4)
            ttk.Label(frame, text=hint).grid(row=row, column=2, sticky=tk.W)
        for row, (variable, label) in enumerate(
            ((self.interval_var, "自動取得間隔 (秒、0.2以上)"),
             (self.wait_var, "各日の公開待ち上限 (秒)")), start=len(fields)
        ):
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky=tk.W, pady=4)
            ttk.Entry(frame, textvariable=variable, width=12).grid(row=row, column=1, padx=12)

    def _scroll_panel(self, parent: tk.Widget, width: int) -> tuple[ttk.Frame, ttk.Frame]:
        panel = ttk.Frame(parent)
        canvas = tk.Canvas(panel, width=width, height=350, highlightthickness=0,
                           background=ttk.Style(self).lookup("TFrame", "background"))
        scrollbar = ttk.Scrollbar(panel, orient=tk.VERTICAL, command=canvas.yview)
        canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        inner = ttk.Frame(canvas, padding=8)
        window = canvas.create_window(0, 0, window=inner, anchor=tk.NW)
        inner.bind("<Configure>", lambda _event: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda event: canvas.itemconfigure(window, width=event.width))
        return panel, inner

    def _path_row(
        self,
        parent: ttk.Frame,
        row: int,
        column: int,
        label: str,
        variable: tk.StringVar,
        command: Callable[[], None],
    ) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=column, sticky=tk.E, padx=(0, 5), pady=2)
        ttk.Entry(parent, textvariable=variable).grid(
            row=row, column=column + 1, sticky=tk.EW, pady=2
        )
        ttk.Button(parent, text="選択", command=command).grid(
            row=row, column=column + 2, sticky=tk.W, padx=(5, 0), pady=2
        )

    def _build_workflow(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(0, weight=1)
        ttk.Label(parent, text="ワークフロー", style="Section.TLabel").grid(
            row=0, column=0, sticky=tk.W, pady=(0, 8)
        )

        actions = [
            ("0  mainをビルド", self._build_solver, False),
            ("1  試合設定を取得", self._fetch_setting, True),
            ("2  エージェント種別を選択", self._select_agents, True),
            ("3  エージェント種別を送信", self._send_agents, False),
        ]
        row = 1
        for text, command, primary in actions:
            button = ttk.Button(
                parent,
                text=text,
                command=command,
                style="Primary.TButton" if primary else "TButton",
            )
            button.grid(row=row, column=0, sticky=tk.EW, pady=3)
            self._action_buttons.append(button)
            if not text.startswith("0 "):
                self._manual_buttons.append(button)
            row += 1

        ttk.Separator(parent).grid(row=row, column=0, sticky=tk.EW, pady=10)
        row += 1
        day_row = ttk.Frame(parent)
        day_row.grid(row=row, column=0, sticky=tk.EW)
        ttk.Label(day_row, text="対象日 day").pack(side=tk.LEFT)
        ttk.Spinbox(day_row, from_=0, to=9, textvariable=self.day_var, width=5).pack(
            side=tk.RIGHT
        )
        row += 1

        for text, command, primary in [
            ("4  当日状態を取得", self._fetch_state, True),
            ("5  日次探索を実行", self._solve_day, True),
            ("6  日次回答を送信", self._send_answer, False),
        ]:
            button = ttk.Button(
                parent,
                text=text,
                command=command,
                style="Primary.TButton" if primary else "TButton",
            )
            button.grid(row=row, column=0, sticky=tk.EW, pady=3)
            self._action_buttons.append(button)
            self._manual_buttons.append(button)
            row += 1

        ttk.Separator(parent).grid(row=row, column=0, sticky=tk.EW, pady=10)
        row += 1
        ttk.Label(parent, text="使用ファイル", style="Section.TLabel").grid(
            row=row, column=0, sticky=tk.W, pady=(0, 4)
        )
        row += 1
        for label, variable, command in [
            ("setting", self.setting_var, self._choose_setting),
            ("state", self.state_var, self._choose_state),
            ("score", self.score_var, self._choose_score),
        ]:
            frame = ttk.Frame(parent)
            frame.grid(row=row, column=0, sticky=tk.EW, pady=2)
            frame.columnconfigure(0, weight=1)
            ttk.Label(frame, text=label).grid(row=0, column=0, sticky=tk.W)
            ttk.Entry(frame, textvariable=variable, width=34).grid(
                row=1, column=0, sticky=tk.EW
            )
            ttk.Button(frame, text="開く", command=command, width=6).grid(
                row=1, column=1, padx=(4, 0)
            )
            row += 1

        local = ttk.Button(
            parent,
            text="ローカル設定を探索",
            command=self._solve_local,
        )
        local.grid(row=row, column=0, sticky=tk.EW, pady=(8, 3))
        self._action_buttons.append(local)
        self._manual_buttons.append(local)
        row += 1
        ttk.Button(parent, text="表示を再読込", command=self._refresh_views).grid(
            row=row, column=0, sticky=tk.EW, pady=3
        )

    def _build_map_tab(self, notebook: ttk.Notebook) -> None:
        frame = ttk.Frame(notebook)
        notebook.add(frame, text="マップ")
        ttk.Label(frame, textvariable=self.map_info_var, anchor=tk.W).pack(
            fill=tk.X, padx=8, pady=6
        )
        canvas_frame = ttk.Frame(frame)
        canvas_frame.pack(fill=tk.BOTH, expand=True)
        self.map_canvas = tk.Canvas(
            canvas_frame,
            background="#f6f7f8",
            highlightthickness=0,
        )
        xbar = ttk.Scrollbar(canvas_frame, orient=tk.HORIZONTAL, command=self.map_canvas.xview)
        ybar = ttk.Scrollbar(canvas_frame, orient=tk.VERTICAL, command=self.map_canvas.yview)
        self.map_canvas.configure(xscrollcommand=xbar.set, yscrollcommand=ybar.set)
        self.map_canvas.grid(row=0, column=0, sticky=tk.NSEW)
        ybar.grid(row=0, column=1, sticky=tk.NS)
        xbar.grid(row=1, column=0, sticky=tk.EW)
        canvas_frame.rowconfigure(0, weight=1)
        canvas_frame.columnconfigure(0, weight=1)

        legend = ttk.Frame(frame, padding=6)
        legend.pack(fill=tk.X)
        for text, color in [
            ("平地", self.TERRAIN_COLORS[0]),
            ("道路", self.TERRAIN_COLORS[1]),
            ("山", self.TERRAIN_COLORS[2]),
            ("池", self.TERRAIN_COLORS[3]),
            ("混雑", self.TRAFFIC_COLORS[1]),
            ("渋滞", self.TRAFFIC_COLORS[2]),
        ]:
            swatch = tk.Canvas(legend, width=15, height=15, highlightthickness=0)
            swatch.create_rectangle(1, 1, 14, 14, fill=color, outline="#68727d")
            swatch.pack(side=tk.LEFT, padx=(7, 2))
            ttk.Label(legend, text=text).pack(side=tk.LEFT)
        ttk.Label(legend, text="  [N]=系列  P=巡回車  S=補給車").pack(side=tk.LEFT, padx=8)

    def _build_json_tab(self, notebook: ttk.Notebook) -> None:
        frame = ttk.Frame(notebook)
        notebook.add(frame, text="JSON / エージェント")
        toolbar = ttk.Frame(frame, padding=6)
        toolbar.pack(fill=tk.X)
        ttk.Button(toolbar, text="setting", command=lambda: self._show_json(Path(self.setting_var.get()))).pack(side=tk.LEFT)
        ttk.Button(toolbar, text="state", command=lambda: self._show_json(Path(self.state_var.get()))).pack(side=tk.LEFT, padx=4)
        ttk.Button(toolbar, text="agents", command=lambda: self._show_json(self._work_path("agents.json"))).pack(side=tk.LEFT)
        ttk.Button(toolbar, text="answer", command=lambda: self._show_json(self._work_path("answer.json"))).pack(side=tk.LEFT, padx=4)
        ttk.Button(toolbar, text="score-next", command=lambda: self._show_json(self._work_path("score-next.json"))).pack(side=tk.LEFT)

        panes = ttk.Panedwindow(frame, orient=tk.VERTICAL)
        panes.pack(fill=tk.BOTH, expand=True)
        table_frame = ttk.Frame(panes)
        panes.add(table_frame, weight=1)
        self.agent_table = ttk.Treeview(
            table_frame,
            columns=("id", "kind", "pos", "coord", "fuel"),
            show="headings",
            height=6,
        )
        for key, title, width in [
            ("id", "ID", 50),
            ("kind", "種別", 90),
            ("pos", "pos", 70),
            ("coord", "座標", 90),
            ("fuel", "燃料", 90),
        ]:
            self.agent_table.heading(key, text=title)
            self.agent_table.column(key, width=width, anchor=tk.CENTER)
        self.agent_table.pack(fill=tk.BOTH, expand=True)

        json_frame = ttk.Frame(panes)
        panes.add(json_frame, weight=3)
        self.json_text = ScrolledText(json_frame, wrap=tk.NONE, state=tk.DISABLED)
        self.json_text.pack(fill=tk.BOTH, expand=True)

    def _workflow(self) -> Workflow:
        try:
            timeout = float(self.timeout_var.get())
            parameters = SearchParameters(**{
                key: int(variable.get()) for key, variable in self.parameter_vars.items()
            })
            parameters.arguments()
        except ValueError as error:
            raise WorkflowError("timeoutは数値、探索パラメータは整数で指定してください") from error
        return Workflow(
            ConnectionConfig(self.url_var.get().strip(), self.token_var.get(), timeout),
            Path(self.solver_var.get()),
            Path(self.work_var.get()),
            parameters,
            self._stop,
        )

    def _workflow_or_dialog(self) -> Workflow | None:
        if self._busy:
            return None
        try:
            return self._workflow()
        except (WorkflowError, OSError) as error:
            messagebox.showerror("実行設定", str(error), parent=self)
            return None

    def _work_path(self, name: str) -> Path:
        return Path(self.work_var.get()).expanduser().resolve() / name

    def _run_task(
        self,
        label: str,
        operation: Callable[[], Any],
        success: Callable[[Any], None] | None = None,
    ) -> None:
        if self._busy:
            return
        self._stop.clear()
        self._active_token = self.token_var.get()
        self._set_busy(True, f"{label}中…")
        self._log(f"▶ {label}")

        def worker() -> None:
            try:
                result = operation()
                self._messages.put(("ok", (label, success), result))
            except Exception as error:  # GUI境界で全例外を表示する。
                self._messages.put(("error", label, error))

        threading.Thread(target=worker, daemon=True).start()

    def _poll_messages(self) -> None:
        try:
            while True:
                kind, context, payload = self._messages.get_nowait()
                if kind == "event":
                    self._auto_event(context, payload)
                    continue
                if kind == "ok":
                    label, callback = context
                    if isinstance(payload, CommandResult) and payload.output:
                        self._log(payload.output)
                    if callback is not None:
                        try:
                            callback(payload)
                        except Exception as error:
                            self._log(f"表示更新エラー: {error}")
                    self._log(f"✓ {label} 完了")
                    self._set_busy(False, f"{label} 完了")
                else:
                    label = context
                    if label == "自動実行" and isinstance(payload, Paused) and self._automatic is not None:
                        self._automatic.retryable = True
                        self.scope_var.set("停止位置から再開")
                    self._log(f"{label}: {payload}")
                    self._set_busy(False, "停止しました" if isinstance(payload, Paused) else f"{label} 失敗")
                    if not isinstance(payload, Paused) and not self._closing:
                        messagebox.showerror(label, self._redact(str(payload)), parent=self)
        except queue.Empty:
            pass
        except Exception as error:
            self._log(f"画面更新エラー: {error}")
        if self._closing and not self._busy:
            self._simple_server.stop()
            self.destroy()
            return
        self.after(100, self._poll_messages)

    def _set_busy(self, busy: bool, status: str) -> None:
        self._busy = busy
        self.status_var.set(status)
        state = tk.DISABLED if busy else tk.NORMAL
        for button in self._action_buttons:
            button.configure(state=state)
        for frame in (self.config_frame, self.simple_frame, self.controls_frame, self.parameters_frame):
            self._set_controls(frame, busy)
        self._mode_changed()
        self.stop_button.configure(state=tk.NORMAL if busy else tk.DISABLED)
        self._sync_simple_server_buttons()

    def _set_controls(self, parent: tk.Widget, disabled: bool) -> None:
        for widget in parent.winfo_children():
            if isinstance(widget, (ttk.Entry, ttk.Button, ttk.Spinbox)):
                widget.configure(state=tk.DISABLED if disabled else tk.NORMAL)
            self._set_controls(widget, disabled)

    def _mode_changed(self, user_changed: bool = False) -> None:
        if user_changed and self._busy and self.mode_var.get() == "manual":
            self._request_stop()
        automatic = self.mode_var.get() == "auto"
        for button in self._manual_buttons:
            button.configure(state=tk.DISABLED if automatic or self._busy else tk.NORMAL)
        self.auto_button.configure(state=tk.NORMAL if automatic and not self._busy else tk.DISABLED)
        self.scope_box.configure(state="readonly" if automatic and not self._busy else tk.DISABLED)

    def _request_stop(self) -> None:
        self._stop.set()
        self.status_var.set("停止要求中… 実行中のHTTP送信は応答を確認してから停止します")

    def _close(self) -> None:
        self._closing = True
        self._simple_server.stop()
        if self._busy:
            self._request_stop()
        else:
            self.destroy()

    def _sync_simple_server_buttons(self) -> None:
        if self._busy:
            return
        running = self._simple_server.running
        self.simple_start_button.configure(state=tk.DISABLED if running else tk.NORMAL)
        self.simple_stop_button.configure(state=tk.NORMAL if running else tk.DISABLED)

    def _reload_simple_teams(self, show_dialog: bool = True) -> None:
        try:
            self._simple_teams = simple_server_teams(Path(self.simple_config_var.get()))
            labels = tuple(name for name, _token in self._simple_teams)
            self.simple_team_box.configure(values=labels)
            if self.simple_team_var.get() not in labels:
                self.simple_team_var.set(labels[0])
        except (WorkflowError, OSError) as error:
            self._simple_teams = ()
            self.simple_team_box.configure(values=())
            self.simple_team_var.set("")
            if show_dialog:
                messagebox.showerror("簡易サーバー設定", str(error), parent=self)

    def _start_simple_server(self) -> None:
        if self._busy:
            return
        try:
            self._reload_simple_teams()
            if not self._simple_teams:
                return
            team = self.simple_team_var.get()
            team_index = [name for name, _token in self._simple_teams].index(team)
            config = Path(self.simple_config_var.get())
            port = int(self.simple_port_var.get())
            kind_deadline = float(self.simple_kind_deadline_var.get())
            start_delay = float(self.simple_start_delay_var.get())
        except (ValueError, tk.TclError) as error:
            messagebox.showerror(
                "簡易サーバー", "portは整数、締切と開始待ちは秒数で指定してください", parent=self
            )
            return

        def started(info: SimpleServerInfo) -> None:
            self.url_var.set(info.url)
            self.token_var.set(info.token)
            client_work = PROJECT_ROOT / "runtime/simple-server/client"
            self.work_var.set(str(client_work))
            self.setting_var.set(str(client_work / "setting.json"))
            self.state_var.set(str(client_work / "state-day0.json"))
            self.score_var.set("")
            self.day_var.set(0)
            self._active_token = info.token
            self._log(
                f"簡易サーバー起動: {info.url} / team={info.team_label}\n"
                f"ログ: {info.log_path}\n通常の通信確認・手動操作・自動操作を使用できます"
            )
            self._sync_simple_server_buttons()

        self._run_task(
            "簡易サーバー起動",
            lambda: self._simple_server.start(
                config, team_index, port, kind_deadline, start_delay
            ),
            started,
        )

    def _stop_simple_server(self) -> None:
        stopped = self._simple_server.stop()
        self._log("簡易サーバーを停止しました" if stopped else "簡易サーバーは停止済みです")
        self.status_var.set("簡易サーバー停止")
        self._sync_simple_server_buttons()

    def _redact(self, message: str) -> str:
        for secret in (self._active_token, self.token_var.get()):
            if secret:
                message = message.replace(secret, "[TOKEN]")
        return message

    def _log(self, message: str) -> None:
        message = self._redact(message)
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, message.rstrip() + "\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)

    def _reload_env(self, show_dialog: bool = True) -> None:
        try:
            config = connection_from_env(Path(self.env_var.get()))
            self.url_var.set(config.url)
            self.token_var.set(config.token)
            status = "TOKEN読込済み" if config.token else "TOKEN未設定"
            self.status_var.set(f".env再読込: {status}")
        except (WorkflowError, OSError) as error:
            if show_dialog:
                messagebox.showerror(".env読込", str(error), parent=self)

    def _build_solver(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        self._run_task("mainのビルド", workflow.build_solver)

    def _check_connection(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is not None:
            self._run_task("通信確認", workflow.check_connection, self._log)

    def _start_auto(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        try:
            workflow.connection.validate()
            if self.scope_var.get() == "停止位置から再開":
                runner = self._automatic
                if runner is None or runner.phase == "done":
                    raise WorkflowError("再開できる自動実行がありません")
                if not runner.retryable:
                    raise WorkflowError("送信が失敗しました。受理状況を確認し、手動操作で続行してください")
                if (workflow.identity() != runner.workflow.identity()
                        or workflow.work_dir != runner.workflow.work_dir):
                    raise WorkflowError("再開時は同じ接続先・TOKEN・作業フォルダを使用してください")
                runner.workflow = workflow
            else:
                day = self._day() if self.scope_var.get().startswith("対象day") else None
                checkpoint = None
                if day is not None and day > 0:
                    checkpoint = Path(self.score_var.get() or self._work_path(f"score-day{day}.json"))
                    if read_json(checkpoint).get("nextDay") != day:
                        raise WorkflowError("開始dayに対応するチェックポイントが必要です")
                runner = AutomaticMatch(
                    workflow, lambda key, data: self._messages.put(("event", key, data)),
                    day, checkpoint, float(self.interval_var.get()), float(self.wait_var.get()),
                )
            self._automatic = runner
            self._run_task("自動実行", runner.run)
        except (WorkflowError, ValueError, OSError, tk.TclError) as error:
            messagebox.showerror("自動実行", str(error), parent=self)

    def _auto_event(self, key: str, data: Any) -> None:
        if key == "phase":
            self.status_var.set(data)
            self._log(data)
        elif key == "log":
            self._log(data)
        elif key == "setting":
            self._setting_loaded(data)
        elif key == "state":
            self.state_var.set(str(data))
            self._state_loaded(data)
        elif key == "agents":
            self._agents_loaded(data)
        elif key == "answer":
            self._show_json(data)
        elif key == "accepted":
            revision, checkpoint, next_day = data
            self.score_var.set(str(checkpoint))
            self.day_var.set(next_day)
            self._log(f"day={next_day - 1} 受理: revision={revision} / 次day={next_day}")

    def _day(self) -> int:
        day = self.day_var.get()
        if not 0 <= day <= 9:
            raise WorkflowError("dayは0〜9で指定してください")
        return day

    def _day_or_dialog(self) -> int | None:
        try:
            return self._day()
        except (WorkflowError, tk.TclError) as error:
            messagebox.showerror("対象日", f"dayは0〜9の整数で指定してください\n{error}", parent=self)
            return None

    def _setting_loaded(self, path: Path) -> None:
        self.setting_var.set(str(path))
        self.state_var.set("")
        self.score_var.set("")
        self._selected_kinds = []
        self._refresh_views()

    def _agents_loaded(self, path: Path) -> None:
        self._selected_kinds = read_json(path)
        self._refresh_views()
        self._show_json(path)

    def _fetch_setting(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        output = self._work_path("setting.json")
        self._run_task(
            "試合設定の取得",
            lambda: workflow.fetch_setting(output),
            self._setting_loaded,
        )

    def _select_agents(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        setting = Path(self.setting_var.get())
        output = self._work_path("agents.json")
        self._run_task(
            "エージェント種別選択",
            lambda: workflow.select_agents(setting, output),
            lambda _: self._agents_loaded(output),
        )

    def _send_agents(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        payload = self._work_path("agents.json")
        if not messagebox.askyesno(
            "種別送信の確認",
            f"回答システムへエージェント種別を送信します。\n\n{payload}\n\n実行しますか？",
            parent=self,
        ):
            return
        setting = Path(self.setting_var.get())
        self._run_task("エージェント種別送信", lambda: workflow.send_agents(payload, setting))

    def _fetch_state(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        day = self._day_or_dialog()
        if day is None:
            return
        output = self._work_path(f"state-day{day}.json")
        self._run_task(
            f"day={day} 状態取得",
            lambda: workflow.fetch_state(day, output),
            lambda path: (self.state_var.set(str(path)), self._state_loaded(path)),
        )

    def _solve_day(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        day = self._day_or_dialog()
        if day is None:
            return
        setting = Path(self.setting_var.get())
        state = Path(self.state_var.get())
        try:
            if read_json(state).get("day") != day:
                raise WorkflowError("対象dayと状態ファイルの日番号が一致しません")
        except (WorkflowError, OSError, AttributeError) as error:
            messagebox.showerror("探索", str(error), parent=self)
            return
        checkpoint: Path | None = None
        if day > 0:
            score_text = self.score_var.get().strip()
            if not score_text:
                score_text = str(self._work_path(f"score-day{day}.json"))
                self.score_var.set(score_text)
            checkpoint = Path(score_text)
        self._run_task(
            f"day={day} 探索",
            lambda: workflow.solve_day(
                setting,
                state,
                checkpoint,
            ),
            lambda _: self._show_json(self._work_path("answer.json")),
        )

    def _solve_local(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        setting = Path(self.setting_var.get())
        self._run_task(
            "ローカル設定の探索",
            lambda: workflow.solve_local(setting),
            lambda _: self._show_json(self._work_path("answer.json")),
        )

    def _send_answer(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        day = self._day_or_dialog()
        if day is None:
            return
        answer = self._work_path("answer.json")
        score = self._work_path("score-next.json")
        if not messagebox.askyesno(
            "回答送信の確認",
            f"day={day} の回答を送信します。\n\n{answer}\n\n受理後に得点チェックポイントを確定します。実行しますか？",
            parent=self,
        ):
            return

        def accepted(result: tuple[int, Path, Path]) -> None:
            revision, archived, checkpoint = result
            self.score_var.set(str(checkpoint))
            self.day_var.set(day + 1)
            self._log(
                f"revision={revision}\n保存: {archived}\n次日チェックポイント: {checkpoint}"
            )

        state = Path(self.state_var.get())
        setting = Path(self.setting_var.get())
        self._run_task(
            f"day={day} 回答送信",
            lambda: workflow.send_answer(day, answer, score, state, setting),
            accepted,
        )

    def _state_loaded(self, path: Path) -> None:
        try:
            value = read_json(path)
            if isinstance(value, dict) and type(value.get("day")) is int:
                day = value["day"]
                self.day_var.set(day)
                if day > 0:
                    candidate = self._work_path(f"score-day{day}.json")
                    if candidate.is_file() or not self.score_var.get().strip():
                        self.score_var.set(str(candidate))
        except WorkflowError:
            pass
        self._refresh_views()

    def _refresh_views(self) -> None:
        setting = Path(self.setting_var.get()).expanduser()
        state_text = self.state_var.get().strip()
        state = Path(state_text).expanduser() if state_text else None
        try:
            snapshot = load_snapshot(setting, state)
        except (WorkflowError, OSError, ValueError, KeyError, TypeError) as error:
            self.map_info_var.set(str(error))
            return
        if snapshot.day is None and len(self._selected_kinds) == len(snapshot.agents):
            snapshot = replace(snapshot, agents=tuple(
                replace(agent, kind=self._selected_kinds[agent.index]) for agent in snapshot.agents
            ))
        self._current_snapshot = snapshot
        self._draw_map(snapshot)
        self._fill_agents(snapshot)
        day_text = "未取得" if snapshot.day is None else str(snapshot.day)
        steps = "-"
        if snapshot.day is not None and snapshot.day < len(snapshot.day_steps):
            steps = str(snapshot.day_steps[snapshot.day])
        self.map_info_var.set(
            f"{snapshot.width}×{snapshot.height}  day={day_text}  "
            f"steps={steps}  agents={len(snapshot.agents)}  fuelLimit={snapshot.fuel_limit}"
        )

    def _draw_map(self, snapshot: MapSnapshot) -> None:
        canvas = self.map_canvas
        canvas.delete("all")
        radius = 18 if max(snapshot.width, snapshot.height) <= 16 else (
            14 if max(snapshot.width, snapshot.height) <= 24 else 11
        )
        hex_width = math.sqrt(3) * radius
        margin = radius + 12
        agent_groups: dict[int, list[Any]] = {}
        for agent in snapshot.agents:
            agent_groups.setdefault(agent.pos, []).append(agent)

        for y, row in enumerate(snapshot.cells):
            for x, terrain in enumerate(row):
                pos = y * snapshot.width + x
                cx = margin + hex_width * (x + (0.5 if y % 2 == 0 else 0.0))
                cy = margin + 1.5 * radius * y
                points = []
                for index in range(6):
                    angle = math.radians(30 + 60 * index)
                    points.extend((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
                color = self.TERRAIN_COLORS.get(terrain, "#eeeeee")
                if terrain == 1 and snapshot.traffics.get(pos, 0) in self.TRAFFIC_COLORS:
                    color = self.TRAFFIC_COLORS[snapshot.traffics[pos]]
                tag = f"cell-{pos}"
                canvas.create_polygon(
                    points,
                    fill=color,
                    outline="#69737d",
                    width=1,
                    tags=(tag,),
                )
                if pos in snapshot.spots:
                    brand, stocks = snapshot.spots[pos]
                    canvas.create_text(
                        cx,
                        cy,
                        text=f"[{brand}]",
                        fill="#174b2d",
                        font=("TkDefaultFont", max(7, radius // 2), "bold"),
                        tags=(tag,),
                    )
                    detail = f"pos={pos} ({x},{y}) spot brand={brand} stocks={stocks}"
                else:
                    detail = f"pos={pos} ({x},{y}) terrain={terrain}"
                canvas.tag_bind(tag, "<Button-1>", lambda _event, value=detail: self.status_var.set(value))

                for offset, agent in enumerate(agent_groups.get(pos, [])):
                    ax = cx - radius * 0.45 + offset * radius * 0.42
                    ay = cy + radius * 0.48
                    label = "P" if agent.kind == 0 else ("S" if agent.kind == 1 else "?")
                    fill = "#235fa4" if agent.kind == 0 else "#7b3fa1"
                    size = max(5, radius * 0.28)
                    canvas.create_oval(ax - size, ay - size, ax + size, ay + size, fill=fill, outline="")
                    canvas.create_text(
                        ax,
                        ay,
                        text=f"{agent.index}{label}",
                        fill="white",
                        font=("TkDefaultFont", max(6, radius // 3), "bold"),
                    )

        total_width = margin * 2 + hex_width * (snapshot.width + 0.5)
        total_height = margin * 2 + 1.5 * radius * max(0, snapshot.height - 1)
        canvas.configure(scrollregion=(0, 0, total_width, total_height))

    def _fill_agents(self, snapshot: MapSnapshot) -> None:
        self.agent_table.delete(*self.agent_table.get_children())
        for agent in snapshot.agents:
            x = agent.pos % snapshot.width
            y = agent.pos // snapshot.width
            self.agent_table.insert(
                "",
                tk.END,
                values=(
                    agent.index,
                    "巡回車" if agent.kind == 0 else ("補給車" if agent.kind == 1 else "未選択"),
                    agent.pos,
                    f"({x},{y})",
                    f"{agent.fuel}/{snapshot.fuel_limit}",
                ),
            )

    def _show_json(self, path: Path) -> None:
        try:
            value = read_json(path)
            text = json.dumps(value, ensure_ascii=False, indent=2)
        except WorkflowError as error:
            messagebox.showerror("JSON表示", str(error), parent=self)
            return
        self.json_text.configure(state=tk.NORMAL)
        self.json_text.delete("1.0", tk.END)
        self.json_text.insert("1.0", text + "\n")
        self.json_text.configure(state=tk.DISABLED)
        self.status_var.set(f"表示: {path}")

    def _choose_env(self) -> None:
        path = filedialog.askopenfilename(parent=self, initialdir=PROJECT_ROOT, title=".envを選択")
        if path:
            self.env_var.set(path)
            self._reload_env()

    def _choose_simple_config(self) -> None:
        path = filedialog.askopenfilename(
            parent=self,
            initialdir=DEFAULT_SIMPLE_CONFIG.parent,
            title="簡易サーバー設定JSONを選択",
            filetypes=[("JSON", "*.json")],
        )
        if path:
            self.simple_config_var.set(path)
            self._reload_simple_teams()

    def _choose_solver(self) -> None:
        path = filedialog.askopenfilename(parent=self, initialdir=PROJECT_ROOT, title="solverを選択")
        if path:
            self.solver_var.set(path)

    def _choose_work_dir(self) -> None:
        path = filedialog.askdirectory(parent=self, initialdir=PROJECT_ROOT, title="作業フォルダを選択")
        if path:
            self.work_var.set(path)

    def _choose_setting(self) -> None:
        path = filedialog.askopenfilename(parent=self, initialdir=PROJECT_ROOT, filetypes=[("JSON", "*.json")])
        if path:
            self._setting_loaded(Path(path))
            self._show_json(Path(path))

    def _choose_state(self) -> None:
        path = filedialog.askopenfilename(parent=self, initialdir=self.work_var.get(), filetypes=[("JSON", "*.json")])
        if path:
            self.state_var.set(path)
            self._state_loaded(Path(path))
            self._show_json(Path(path))

    def _choose_score(self) -> None:
        path = filedialog.askopenfilename(parent=self, initialdir=self.work_var.get(), filetypes=[("JSON", "*.json")])
        if path:
            self.score_var.set(path)
            self._show_json(Path(path))


def main() -> None:
    app = SolverGUI()
    app.mainloop()


if __name__ == "__main__":
    main()
