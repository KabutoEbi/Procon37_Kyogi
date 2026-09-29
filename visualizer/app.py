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
import time
from dataclasses import replace
from datetime import datetime
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any, Callable

from backend import (
    PROJECT_ROOT,
    CommandResult,
    ConnectionConfig,
    MapSnapshot,
    Workflow,
    WorkflowError,
    SearchParameters,
    RandomSettingParameters,
    SimpleServer,
    SimpleServerInfo,
    SolverResultSummary,
    trace_answer_routes,
    DEFAULT_SIMPLE_CONFIG,
    Paused,
    connection_from_env,
    load_snapshot,
    map_acquisition_totals,
    parse_solver_result,
    read_json,
    simple_server_teams,
)


class AutoScrollbar(ttk.Scrollbar):
    """内容が表示領域に収まるときは場所を取らないスクロールバー。"""

    def set(self, first: str, last: str) -> None:
        if float(first) <= 0.0 and float(last) >= 1.0:
            self.grid_remove()
        else:
            self.grid()
        super().set(first, last)


class SolverGUI(tk.Tk):
    TERRAIN_COLORS = {
        0: "#e9e4d4",
        1: "#cfd5dc",
        2: "#a98c6b",
        3: "#7fc4de",
    }
    TRAFFIC_COLORS = {1: "#f2b84b", 2: "#df6657"}
    ROUTE_COLORS = (
        "#d62728", "#1f77b4", "#2ca02c", "#9467bd",
        "#ff7f0e", "#17becf", "#8c564b", "#e377c2",
    )

    def __init__(self) -> None:
        if not supported_tk():
            raise RuntimeError("このPythonのTkはGUI表示に非対応です。python3 visualizer/app.pyで起動してください")
        super().__init__()
        self.title("ビジュアライザ")
        self.geometry("1380x880")
        self.minsize(1040, 700)

        self._messages: queue.Queue[tuple[str, Any, Any]] = queue.Queue()
        self._busy = False
        self._action_buttons: list[ttk.Button] = []
        self._current_snapshot: MapSnapshot | None = None
        self._answer_routes: tuple[tuple[int, ...], ...] | None = None
        self._answer_route_day: int | None = None
        self._day_count = 0
        self._map_redraw_job: str | None = None
        self._stop = threading.Event()
        self._closing = False
        self._active_token = ""
        self._selected_kinds: list[int] = []
        self._logged_schedule: tuple[int, tuple[int, ...]] | None = None
        self._milestones: dict[str, tuple[float, str]] = {}
        self._simple_server = SimpleServer(PROJECT_ROOT / "runtime/simple-server")
        self._simple_teams: tuple[tuple[str, str], ...] = ()
        self.mode_var = tk.StringVar(value="manual")
        self.parameter_vars = {
            name: tk.StringVar(value=str(value))
            for name, value in vars(SearchParameters()).items()
        }
        self.random_setting_vars = {
            name: tk.StringVar(value=str(value))
            for name, value in vars(RandomSettingParameters()).items()
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
        self.search_result_vars = {
            "status": tk.StringVar(value="未実行"),
            "target": tk.StringVar(value="-"),
            "elapsed": tk.StringVar(value="-"),
            "expanded": tk.StringVar(value="-"),
            "daily_count": tk.StringVar(value="-"),
            "total_count": tk.StringVar(value="-"),
            "daily_brands": tk.StringVar(value="-"),
            "total_brands": tk.StringVar(value="-"),
        }
        self.simple_config_var = tk.StringVar(value=str(DEFAULT_SIMPLE_CONFIG))
        self.simple_visible_var = tk.BooleanVar(value=False)
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
        simple_row = ttk.Frame(self, padding=(12, 8))
        self.simple_row = simple_row
        simple_row.pack(fill=tk.X)
        simple_row.columnconfigure(1, weight=1)
        self.simple_toggle_check = ttk.Checkbutton(
            simple_row,
            text="簡易サーバー",
            variable=self.simple_visible_var,
            command=self._toggle_simple_server,
        )
        self.simple_toggle_check.grid(row=0, column=0, sticky=tk.W, padx=(0, 8))
        self.settings_button = ttk.Button(
            simple_row,
            text="実行設定・使用ファイル",
            command=self._show_settings_window,
        )
        self.settings_button.grid(row=0, column=2, sticky=tk.E, padx=(8, 0))

        simple = ttk.Frame(simple_row)
        self.simple_frame = simple
        simple.grid(row=0, column=1, sticky=tk.EW)
        simple.grid_remove()
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
        self.simple_stop_button.grid(row=0, column=12, padx=(0, 5))
        self.simple_clear_button = ttk.Button(
            simple,
            text="作業フォルダをクリア",
            command=self._clear_simple_server_work,
        )
        self.simple_clear_button.grid(row=0, column=13)
        self._action_buttons.append(self.simple_start_button)

        mode = ttk.Frame(self, padding=(12, 0, 12, 8))
        mode.pack(fill=tk.X)
        ttk.Label(mode, text="日次回答の送信").pack(side=tk.LEFT, padx=(0, 8))
        for value, label in (("manual", "手動"), ("auto", "探索後に自動")):
            ttk.Radiobutton(mode, text=label, variable=self.mode_var, value=value,
                            command=self._mode_changed).pack(side=tk.LEFT, padx=4)
        self.stop_button = ttk.Button(mode, text="停止", command=self._request_stop, state=tk.DISABLED)
        self.stop_button.pack(side=tk.LEFT, padx=6)
        ttk.Label(mode, text="dayは0始まり / 設定変更は次の実行から適用").pack(side=tk.LEFT, padx=10)

        content = ttk.Panedwindow(self, orient=tk.VERTICAL)
        content.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 8))
        self.content_pane = content
        main = ttk.Panedwindow(content, orient=tk.HORIZONTAL)
        self.main_pane = main
        content.add(main, weight=4)

        control_panel = ttk.Frame(main, width=260)
        self.control_panel = control_panel
        control_panel.columnconfigure(0, weight=1)
        controls = ttk.Frame(control_panel, padding=8)
        controls.grid(row=0, column=0, sticky=tk.NSEW)
        self.controls_frame = controls
        main.add(control_panel, weight=0)
        self._build_workflow(controls)

        notebook = ttk.Notebook(main)
        main.add(notebook, weight=1)
        self._build_map_tab(notebook)
        self._build_json_tab(notebook)
        self._build_parameters_tab(notebook)
        self._build_log_panel(content)

        status = ttk.Label(self, textvariable=self.status_var, style="Status.TLabel", anchor=tk.W)
        status.pack(fill=tk.X, padx=8, pady=(0, 5))
        self._build_settings_window()
        self._mode_changed()

    def _build_settings_window(self) -> None:
        window = tk.Toplevel(self)
        self.settings_window = window
        window.title("Procon37 実行設定・使用ファイル")
        window.geometry("1000x450")
        window.minsize(820, 410)
        window.protocol("WM_DELETE_WINDOW", self._hide_settings_window)

        header = ttk.Frame(window, padding=(12, 10, 12, 6))
        header.pack(fill=tk.X)
        ttk.Label(
            header,
            text="実行設定・使用ファイル",
            style="Title.TLabel",
        ).pack(side=tk.LEFT)
        ttk.Button(
            header,
            text="閉じる",
            command=self._hide_settings_window,
        ).pack(side=tk.RIGHT)

        config = ttk.LabelFrame(window, text="実行設定", padding=8)
        self.config_frame = config
        config.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 12))
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
        self.token_entry = ttk.Entry(config, textvariable=self.token_var)
        self.token_entry.grid(row=2, column=1, columnspan=2, sticky=tk.EW)
        ttk.Label(config, text="timeout(s)").grid(row=2, column=3, sticky=tk.E, padx=(12, 5))
        ttk.Entry(config, textvariable=self.timeout_var, width=8).grid(row=2, column=4, sticky=tk.W)
        check = ttk.Button(config, text="通信確認（手動）", command=self._check_connection)
        check.grid(row=2, column=5, sticky=tk.E)
        self._action_buttons.append(check)
        self._build_file_panel(config)
        self._build_random_setting_panel(config)
        window.withdraw()

    def _show_settings_window(self) -> None:
        self.settings_window.deiconify()
        self.settings_window.lift()
        self.settings_window.focus_force()

    def _hide_settings_window(self) -> None:
        self.settings_window.withdraw()
        self.lift()

    def _toggle_simple_server(self) -> None:
        if self.simple_visible_var.get():
            self.simple_frame.grid()
        else:
            self.simple_frame.grid_remove()

    def _build_file_panel(self, parent: ttk.LabelFrame) -> None:
        files = ttk.LabelFrame(parent, text="使用ファイル", padding=6)
        self.files_frame = files
        files.grid(row=3, column=0, columnspan=6, sticky=tk.EW, pady=(8, 0))
        files.columnconfigure(1, weight=1)
        files.columnconfigure(4, weight=1)
        self._path_row(files, 0, 0, "setting", self.setting_var, self._choose_setting)
        self._path_row(files, 0, 3, "state", self.state_var, self._choose_state)
        self._path_row(files, 1, 0, "score", self.score_var, self._choose_score)
        local = ttk.Button(files, text="ローカル設定を探索", command=self._solve_local)
        local.grid(row=1, column=3, columnspan=2, sticky=tk.EW, padx=(0, 5), pady=2)
        self._action_buttons.append(local)
        ttk.Button(files, text="表示を再読込", command=self._refresh_views).grid(
            row=1, column=5, sticky=tk.EW, pady=2
        )

    def _build_random_setting_panel(self, parent: ttk.LabelFrame) -> None:
        frame = ttk.LabelFrame(parent, text="ランダム設定", padding=6)
        self.random_setting_frame = frame
        frame.grid(row=4, column=0, columnspan=6, sticky=tk.EW, pady=(8, 0))
        fields = [
            ("width", "幅"), ("height", "高さ"), ("agents", "エージェント"),
            ("spots", "スポット"), ("brands", "系列"), ("days", "日数"),
            ("seconds", "1日秒数"), ("teams", "チーム"), ("seed", "seed"),
        ]
        for index, (key, label) in enumerate(fields):
            row, group = divmod(index, 3)
            column = group * 2
            ttk.Label(frame, text=label).grid(
                row=row, column=column, sticky=tk.E, padx=(6, 4), pady=2
            )
            ttk.Entry(
                frame, textvariable=self.random_setting_vars[key], width=10
            ).grid(row=row, column=column + 1, sticky=tk.EW, pady=2)
            frame.columnconfigure(column + 1, weight=1)
        generate = ttk.Button(
            frame,
            text="ランダム生成して探索",
            command=self._generate_and_solve_local,
            style="Primary.TButton",
        )
        generate.grid(row=3, column=0, columnspan=6, sticky=tk.EW, padx=6, pady=(6, 2))
        self.random_generate_button = generate
        self._action_buttons.append(generate)

    def _build_parameters_tab(self, notebook: ttk.Notebook) -> None:
        frame = ttk.Frame(notebook, padding=8)
        notebook.add(frame, text="探索パラメータ")
        self.parameters_frame = frame
        fields = [
            ("beam_width", "ビーム幅", "各展開で保持する状態数"),
            ("joint_beam_width", "同時行動ビーム幅", "複数台への行動割当の幅"),
            ("threads", "スレッド数", "0: 自動 / 1: 直列"),
            ("time_limit_ms", "探索時間上限 (ms)", "0: 自動。各日の試合時間とサーバー締切は常に有効"),
            ("safety_margin_ms", "締切余裕 (ms)", "探索・種別選択を締切より早く終了"),
            ("max_actions", "各車の行動候補数", "0: 全候補"),
            ("macro_actions", "マクロ行動", "1: 有効 / 0: 無効"),
            ("adaptive_beam", "ビーム幅自動調整", "1: 有効 / 0: 指定幅を使用"),
            ("trial_candidates", "種別選択の試走候補数", "0: 距離評価のみ"),
            ("trial_beam_width", "種別選択の試走ビーム幅", "1以上"),
            ("trial_joint_beam_width", "種別選択の同時行動幅", "1以上"),
            ("selection_time_ms", "種別選択時間上限 (ms)", "0: 相対制限なし"),
            ("send_reserve_ms", "送信確保時間 (ms)", "通信timeout＋保存余裕以上を自動確保"),
            ("clock_margin_ms", "時刻余裕 (ms)", "OSとサーバーの時刻ずれ用"),
        ]
        frame.columnconfigure(0, weight=1, uniform="parameter")
        frame.columnconfigure(1, weight=1, uniform="parameter")
        daily = ttk.LabelFrame(frame, text="日次探索", padding=8)
        selection = ttk.LabelFrame(frame, text="種別選択・送信", padding=8)
        daily.grid(row=0, column=0, sticky=tk.NSEW, padx=(0, 4))
        selection.grid(row=0, column=1, sticky=tk.NSEW, padx=(4, 0))

        def add_fields(parent: ttk.LabelFrame, values: list[tuple[str, str, str]]) -> None:
            parent.columnconfigure(2, weight=1)
            for row, (key, label, hint) in enumerate(values):
                ttk.Label(parent, text=label).grid(row=row, column=0, sticky=tk.W, pady=4)
                ttk.Entry(parent, textvariable=self.parameter_vars[key], width=10).grid(
                    row=row, column=1, padx=8, pady=4
                )
                ttk.Label(parent, text=hint, wraplength=230).grid(
                    row=row, column=2, sticky=tk.W
                )

        add_fields(daily, fields[:8])
        add_fields(selection, fields[8:])

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
            ("1  設定取得・種別選択・送信", self._prepare_match, True),
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
            row += 1

        ttk.Separator(parent).grid(row=row, column=0, sticky=tk.EW, pady=8)
        row += 1
        day_row = ttk.Frame(parent)
        day_row.grid(row=row, column=0, sticky=tk.EW)
        ttk.Label(day_row, text="対象日 day").pack(side=tk.LEFT)
        self.day_spinbox = ttk.Spinbox(
            day_row, from_=0, to=9, textvariable=self.day_var, width=5
        )
        self.day_spinbox.pack(side=tk.RIGHT)
        row += 1

        for text, command, primary in [
            ("2  当日状態を取得・探索", self._fetch_and_solve_day, True),
            ("3  日次回答を送信", self._send_answer, False),
        ]:
            button = ttk.Button(
                parent,
                text=text,
                command=command,
                style="Primary.TButton" if primary else "TButton",
            )
            button.grid(row=row, column=0, sticky=tk.EW, pady=3)
            self._action_buttons.append(button)
            if command == self._send_answer:
                self.send_answer_button = button
            row += 1

        result = ttk.LabelFrame(parent, text="探索結果", padding=7)
        self.search_result_frame = result
        result.grid(row=row, column=0, sticky=tk.EW, pady=(14, 0))
        result.columnconfigure(1, weight=1)
        for result_row, (key, label) in enumerate([
            ("status", "状態"),
            ("target", "対象"),
            ("elapsed", "探索時間"),
            ("expanded", "展開ノード"),
            ("daily_count", "当日総獲得数"),
            ("total_count", "全日程合計獲得数"),
            ("daily_brands", "当日獲得系列"),
            ("total_brands", "全日程獲得系列"),
        ]):
            ttk.Label(result, text=label).grid(
                row=result_row, column=0, sticky=tk.W, pady=2
            )
            ttk.Label(
                result,
                textvariable=self.search_result_vars[key],
                anchor=tk.E,
            ).grid(row=result_row, column=1, sticky=tk.EW, padx=(8, 0), pady=2)

    def _build_log_panel(self, parent: ttk.Panedwindow) -> None:
        frame = ttk.LabelFrame(parent, text="実行ログ", padding=5)
        self.log_frame = frame
        parent.add(frame, weight=1)
        frame.rowconfigure(0, weight=1)
        frame.columnconfigure(0, weight=1)
        self.log_text = tk.Text(frame, wrap=tk.WORD, state=tk.DISABLED, height=8)
        scrollbar = AutoScrollbar(frame, orient=tk.VERTICAL, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scrollbar.set)
        self.log_text.grid(row=0, column=0, sticky=tk.NSEW)
        scrollbar.grid(row=0, column=1, sticky=tk.NS)

    def _build_map_tab(self, notebook: ttk.Notebook) -> None:
        frame = ttk.Frame(notebook)
        notebook.add(frame, text="マップ")
        ttk.Label(frame, textvariable=self.map_info_var, anchor=tk.W).pack(
            fill=tk.X, padx=8, pady=6
        )
        body = ttk.Panedwindow(frame, orient=tk.HORIZONTAL)
        body.pack(fill=tk.BOTH, expand=True)
        map_panel = ttk.Frame(body)
        state_panel = ttk.Frame(body, width=330, padding=(6, 0, 0, 0))
        self.map_state_panel = state_panel
        body.add(map_panel, weight=3)
        body.add(state_panel, weight=1)

        canvas_frame = ttk.Frame(map_panel)
        canvas_frame.pack(fill=tk.BOTH, expand=True)
        self.map_canvas = tk.Canvas(
            canvas_frame,
            background="#f6f7f8",
            highlightthickness=0,
        )
        xbar = AutoScrollbar(canvas_frame, orient=tk.HORIZONTAL, command=self.map_canvas.xview)
        ybar = AutoScrollbar(canvas_frame, orient=tk.VERTICAL, command=self.map_canvas.yview)
        self.map_xbar = xbar
        self.map_ybar = ybar
        self.map_canvas.configure(xscrollcommand=xbar.set, yscrollcommand=ybar.set)
        self.map_canvas.bind("<Configure>", self._map_resized)
        self.map_canvas.grid(row=0, column=0, sticky=tk.NSEW)
        ybar.grid(row=0, column=1, sticky=tk.NS)
        xbar.grid(row=1, column=0, sticky=tk.EW)
        canvas_frame.rowconfigure(0, weight=1)
        canvas_frame.columnconfigure(0, weight=1)

        legend = ttk.Frame(map_panel, padding=6)
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

        agents_frame = ttk.LabelFrame(state_panel, text="エージェント", padding=4)
        agents_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 4))
        agents_frame.rowconfigure(0, weight=1)
        agents_frame.columnconfigure(0, weight=1)
        self.agent_table = ttk.Treeview(
            agents_frame,
            columns=("id", "kind", "pos", "coord", "fuel"),
            show="headings",
            height=6,
        )
        for key, title, width in [
            ("id", "ID", 36),
            ("kind", "種別", 70),
            ("pos", "pos", 50),
            ("coord", "座標", 65),
            ("fuel", "燃料", 65),
        ]:
            self.agent_table.heading(key, text=title)
            self.agent_table.column(key, width=width, minwidth=35, anchor=tk.CENTER)
        agent_scroll = AutoScrollbar(
            agents_frame, orient=tk.VERTICAL, command=self.agent_table.yview
        )
        self.agent_table.configure(yscrollcommand=agent_scroll.set)
        self.agent_table.grid(row=0, column=0, sticky=tk.NSEW)
        agent_scroll.grid(row=0, column=1, sticky=tk.NS)

        spots_frame = ttk.LabelFrame(state_panel, text="スポット", padding=4)
        spots_frame.pack(fill=tk.BOTH, expand=True, pady=(4, 0))
        spots_frame.rowconfigure(0, weight=1)
        spots_frame.columnconfigure(0, weight=1)
        self.spot_table = ttk.Treeview(
            spots_frame,
            columns=("pos", "coord", "brand", "stocks"),
            show="headings",
            height=8,
        )
        for key, title, width in [
            ("pos", "pos", 50),
            ("coord", "座標", 70),
            ("brand", "系列", 55),
            ("stocks", "在庫", 55),
        ]:
            self.spot_table.heading(key, text=title)
            self.spot_table.column(key, width=width, minwidth=40, anchor=tk.CENTER)
        spot_scroll = AutoScrollbar(
            spots_frame, orient=tk.VERTICAL, command=self.spot_table.yview
        )
        self.spot_table.configure(yscrollcommand=spot_scroll.set)
        self.spot_table.grid(row=0, column=0, sticky=tk.NSEW)
        spot_scroll.grid(row=0, column=1, sticky=tk.NS)

    def _build_json_tab(self, notebook: ttk.Notebook) -> None:
        frame = ttk.Frame(notebook)
        notebook.add(frame, text="JSON")
        toolbar = ttk.Frame(frame, padding=6)
        toolbar.pack(fill=tk.X)
        ttk.Button(toolbar, text="setting", command=lambda: self._show_json(Path(self.setting_var.get()))).pack(side=tk.LEFT)
        ttk.Button(toolbar, text="state", command=lambda: self._show_json(Path(self.state_var.get()))).pack(side=tk.LEFT, padx=4)
        ttk.Button(toolbar, text="agents", command=lambda: self._show_json(self._work_path("agents.json"))).pack(side=tk.LEFT)
        ttk.Button(toolbar, text="answer", command=lambda: self._show_json(self._work_path("answer.json"))).pack(side=tk.LEFT, padx=4)
        ttk.Button(toolbar, text="score-next", command=lambda: self._show_json(self._work_path("score-next.json"))).pack(side=tk.LEFT)

        json_frame = ttk.Frame(frame)
        json_frame.pack(fill=tk.BOTH, expand=True)
        json_frame.rowconfigure(0, weight=1)
        json_frame.columnconfigure(0, weight=1)
        self.json_text = tk.Text(json_frame, wrap=tk.NONE, state=tk.DISABLED)
        xbar = AutoScrollbar(json_frame, orient=tk.HORIZONTAL, command=self.json_text.xview)
        ybar = AutoScrollbar(json_frame, orient=tk.VERTICAL, command=self.json_text.yview)
        self.json_text.configure(xscrollcommand=xbar.set, yscrollcommand=ybar.set)
        self.json_text.grid(row=0, column=0, sticky=tk.NSEW)
        ybar.grid(row=0, column=1, sticky=tk.NS)
        xbar.grid(row=1, column=0, sticky=tk.EW)

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

    def _random_setting_parameters(self) -> RandomSettingParameters:
        try:
            parameters = RandomSettingParameters(**{
                key: int(variable.get())
                for key, variable in self.random_setting_vars.items()
            })
            parameters.arguments()
            return parameters
        except ValueError as error:
            raise WorkflowError("ランダム設定は整数で指定してください") from error

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
        failure: Callable[[Exception], None] | None = None,
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
                self._messages.put(("error", (label, failure), error))

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
                    label, callback = context
                    self._log(f"{label}: {payload}")
                    if callback is not None:
                        try:
                            callback(payload)
                        except Exception as error:
                            self._log(f"表示更新エラー: {error}")
                    self._set_busy(False, "停止しました" if isinstance(payload, Paused) else f"{label} 失敗")
                    if not isinstance(payload, Paused) and not self._closing:
                        messagebox.showerror(label, self._redact(str(payload)), parent=self)
        except queue.Empty:
            pass
        except Exception as error:
            self._log(f"画面更新エラー: {error}")
        self._emit_due_milestones()
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

    def _mode_changed(self) -> None:
        self.send_answer_button.configure(
            state=tk.DISABLED if self._busy or self.mode_var.get() == "auto" else tk.NORMAL
        )

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
        self.simple_clear_button.configure(state=tk.DISABLED if running else tk.NORMAL)

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
            client_work = self._simple_server.client_work_dir
            self.work_var.set(str(client_work))
            self.setting_var.set(str(client_work / "setting.json"))
            self.state_var.set(str(client_work / "state-day0.json"))
            self.score_var.set("")
            self.day_var.set(0)
            self._active_token = info.token
            self._set_simple_server_milestones(info)
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
        self._milestones.clear()
        self._log("簡易サーバーを停止しました" if stopped else "簡易サーバーは停止済みです")
        self.status_var.set("簡易サーバー停止")
        self._sync_simple_server_buttons()

    def _clear_simple_server_work(self) -> None:
        if self._busy:
            return
        client_work = self._simple_server.client_work_dir
        if not messagebox.askyesno(
            "簡易サーバー作業フォルダのクリア",
            f"次のフォルダ内にある取得データ・回答・送信記録を削除します。\n"
            f"復旧には使用できなくなります。\n\n{client_work}\n\n実行しますか？",
            parent=self,
        ):
            return
        try:
            removed = self._simple_server.clear_client_work()
        except WorkflowError as error:
            messagebox.showerror("簡易サーバー作業フォルダのクリア", str(error), parent=self)
            return
        self.work_var.set(str(client_work))
        self.setting_var.set(str(client_work / "setting.json"))
        self.state_var.set(str(client_work / "state-day0.json"))
        self.score_var.set("")
        self.day_var.set(0)
        self._selected_kinds = []
        self._day_count = 0
        self._current_snapshot = None
        self._logged_schedule = None
        self._milestones.clear()
        self._clear_answer_route()
        self._clear_search_result()
        self.map_canvas.delete("all")
        self.agent_table.delete(*self.agent_table.get_children())
        self.spot_table.delete(*self.spot_table.get_children())
        self.map_info_var.set("試合設定を取得または読み込んでください")
        self._log(f"簡易サーバー作業フォルダをクリアしました: {client_work} ({removed}項目)")
        self.status_var.set("簡易サーバー作業フォルダをクリアしました")

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

    def _set_simple_server_milestones(self, info: SimpleServerInfo) -> None:
        self._milestones.clear()
        if info.selection_ends_at > 0:
            self._milestones["selection-end"] = (
                info.selection_ends_at, "選択時間終了"
            )
        if info.match_starts_at <= 0:
            return
        self._milestones["match-start"] = (info.match_starts_at, "試合開始")
        ends_at = info.match_starts_at
        for day, seconds in enumerate(info.day_seconds, start=1):
            ends_at += seconds
            self._milestones[f"day-{day}-end"] = (
                ends_at, f"{day}日目終了"
            )

    def _set_setting_milestones(
        self, starts_at: int, day_seconds: tuple[int, ...]
    ) -> None:
        if starts_at <= 0:
            return
        self._milestones["match-start"] = (float(starts_at), "試合開始")
        ends_at = float(starts_at)
        for day, seconds in enumerate(day_seconds, start=1):
            ends_at += seconds
            self._milestones[f"day-{day}-end"] = (
                ends_at, f"{day}日目終了"
            )

    def _emit_due_milestones(self) -> None:
        now = time.time()
        due = sorted(
            (when, key, label)
            for key, (when, label) in self._milestones.items()
            if when <= now
        )
        for _when, key, label in due:
            self._milestones.pop(key, None)
            self._log(f"⏰ {label}")

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

    def _prepare_match(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        setting = self._work_path("setting.json")
        agents = self._work_path("agents.json")

        def prepare() -> CommandResult:
            workflow.fetch_setting(setting)
            self._messages.put(("event", "setting", setting))
            result = workflow.select_agents(setting, agents)
            self._messages.put(("event", "agents", agents))
            workflow.send_agents(agents, setting)
            return result

        self._run_task("設定取得・種別選択・送信", prepare)

    def _auto_event(self, key: str, data: Any) -> None:
        if key == "setting":
            self._setting_loaded(data)
        elif key == "agents":
            self._agents_loaded(data)
        elif key == "solve_start":
            self._clear_answer_route()

    def _day(self) -> int:
        day = self.day_var.get()
        maximum = self._day_count - 1 if self._day_count else 9
        if not 0 <= day <= maximum:
            raise WorkflowError(f"dayは0〜{maximum}で指定してください")
        return day

    def _day_or_dialog(self) -> int | None:
        try:
            return self._day()
        except (WorkflowError, tk.TclError) as error:
            messagebox.showerror("対象日", str(error), parent=self)
            return None

    def _setting_loaded(self, path: Path) -> None:
        self._clear_answer_route()
        self._clear_search_result()
        self.setting_var.set(str(path))
        self.state_var.set("")
        self.score_var.set("")
        self._selected_kinds = []
        self._day_count = 0
        self.day_spinbox.configure(to=9)
        self.day_var.set(0)
        self._refresh_views()
        self._log_match_schedule(path)

    def _log_match_schedule(self, path: Path) -> None:
        try:
            setting = read_json(path)
        except WorkflowError:
            return
        if not isinstance(setting, dict):
            return
        starts_at = setting.get("startsAt")
        raw_seconds = setting.get("daySeconds")
        if type(starts_at) is not int or not isinstance(raw_seconds, list) or any(
            type(seconds) is not int or seconds <= 0 for seconds in raw_seconds
        ):
            return
        day_seconds = tuple(raw_seconds)
        signature = (starts_at, day_seconds)
        if signature == self._logged_schedule:
            return
        self._logged_schedule = signature
        self._set_setting_milestones(starts_at, day_seconds)

        if starts_at <= 0:
            return

        def timestamp_text(value: int) -> str:
            try:
                return datetime.fromtimestamp(value).astimezone().strftime(
                    "%Y-%m-%d %H:%M:%S %Z"
                )
            except (OverflowError, OSError, ValueError):
                return f"UNIX {value}"

        self._log("⏱ 試合日程")
        self._log(f"  試合開始予定: {timestamp_text(starts_at)}")
        ends_at = starts_at
        for day, seconds in enumerate(day_seconds, start=1):
            ends_at += seconds
            self._log(f"  {day}日目終了予定: {timestamp_text(ends_at)}")

    def _agents_loaded(self, path: Path) -> None:
        self._selected_kinds = read_json(path)
        self._refresh_views()
        self._show_json(path)

    def _fetch_and_solve_day(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        day = self._day_or_dialog()
        if day is None:
            return
        auto_send = self.mode_var.get() == "auto"
        self._begin_search_result(f"day={day}")
        setting = Path(self.setting_var.get())
        state = self._work_path(f"state-day{day}.json")
        checkpoint: Path | None = None
        if day > 0:
            score_text = self.score_var.get().strip()
            if not score_text:
                score_text = str(self._work_path(f"score-day{day}.json"))
                self.score_var.set(score_text)
            checkpoint = Path(score_text)

        def fetch_and_solve() -> CommandResult:
            workflow.fetch_state(day, state)
            self._messages.put(("event", "solve_start", day))
            return workflow.solve_day(setting, state, checkpoint)

        def completed(result: CommandResult) -> None:
            self.state_var.set(str(state))
            self._state_loaded(state)
            self._show_answer_route(self._work_path("answer.json"))
            self._show_search_result(result)
            self._show_json(self._work_path("answer.json"))
            if auto_send:
                self.after_idle(
                    lambda: self._submit_answer(workflow, day, state, setting, confirm=False)
                )

        self._run_task(
            f"day={day} 状態取得・探索",
            fetch_and_solve,
            completed,
            self._search_result_failed,
        )

    def _solve_local(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        setting = Path(self.setting_var.get())
        self._clear_answer_route()
        self._begin_search_result("ローカル")

        def completed(result: CommandResult) -> None:
            self._setting_loaded(setting)
            initial_snapshot = self._current_snapshot
            result_state = self._work_path("result-state.json")
            self.state_var.set(str(result_state))
            self._state_loaded(result_state)
            self._show_answer_route(
                self._work_path("answer.json"), source_snapshot=initial_snapshot
            )
            self._show_search_result(result, local=True)
            self._show_json(self._work_path("answer.json"))

        self._run_task(
            "ローカル設定の探索",
            lambda: workflow.solve_local(setting),
            completed,
            self._search_result_failed,
        )

    def _generate_and_solve_local(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        try:
            parameters = self._random_setting_parameters()
        except WorkflowError as error:
            messagebox.showerror("ランダム設定", str(error), parent=self)
            return
        setting = self._work_path("random-setting.json")

        def completed(_result: CommandResult) -> None:
            self._setting_loaded(setting)
            self._show_json(setting)
            self.after_idle(self._solve_local)

        self._run_task(
            "ランダム設定生成",
            lambda: workflow.generate_random_setting(setting, parameters),
            completed,
        )

    def _send_answer(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        day = self._day_or_dialog()
        if day is None:
            return
        self._submit_answer(
            workflow, day, Path(self.state_var.get()), Path(self.setting_var.get()),
            confirm=True,
        )

    def _submit_answer(
        self, workflow: Workflow, day: int, state: Path, setting: Path, *, confirm: bool
    ) -> None:
        answer = workflow.work_dir / "answer.json"
        score = workflow.work_dir / "score-next.json"
        if confirm and not messagebox.askyesno(
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
        self._day_count = len(snapshot.day_steps)
        self.day_spinbox.configure(to=max(0, self._day_count - 1))
        self._draw_map(snapshot)
        self._fill_agents(snapshot)
        self._fill_spots(snapshot)
        day_text = "未取得" if snapshot.day is None else str(snapshot.day)
        steps = "-"
        if snapshot.day is not None and snapshot.day < len(snapshot.day_steps):
            steps = str(snapshot.day_steps[snapshot.day])
        days_text = str(len(snapshot.day_steps)) if snapshot.day_steps else "未取得"
        self.map_info_var.set(
            f"{snapshot.width}×{snapshot.height}  day={day_text}  days={days_text}  "
            f"steps={steps}  agents={len(snapshot.agents)}  fuelLimit={snapshot.fuel_limit}"
        )

    def _clear_answer_route(self) -> None:
        self._answer_routes = None
        self._answer_route_day = None
        self.map_canvas.delete("answer-route")

    def _clear_search_result(self) -> None:
        for key, value in {
            "status": "未実行",
            "target": "-",
            "elapsed": "-",
            "expanded": "-",
            "daily_count": "-",
            "total_count": "-",
            "daily_brands": "-",
            "total_brands": "-",
        }.items():
            self.search_result_vars[key].set(value)

    def _begin_search_result(self, target: str) -> None:
        self._clear_search_result()
        self.search_result_vars["status"].set("探索中…")
        self.search_result_vars["target"].set(target)

    def _search_result_failed(self, error: Exception) -> None:
        self.search_result_vars["status"].set(
            "停止" if isinstance(error, Paused) else "失敗"
        )

    def _show_search_result(
        self, result: CommandResult, *, local: bool = False
    ) -> None:
        summary: SolverResultSummary | None = parse_solver_result(result.stdout)
        if summary is None:
            self.search_result_vars["status"].set("結果を読めません")
            return
        stop = {
            "completed": "完了",
            "time-limit": "時間上限",
            "no-valid-plan": "有効解なし",
        }.get(summary.stop, summary.stop)
        self.search_result_vars["status"].set(stop)
        prefix = "ローカル " if local else ""
        self.search_result_vars["target"].set(f"{prefix}day={summary.day}")
        self.search_result_vars["elapsed"].set(f"{summary.elapsed_ms:,} ms")
        self.search_result_vars["expanded"].set(f"{summary.expanded:,}")
        try:
            total_udon, total_brands = map_acquisition_totals(
                Path(self.setting_var.get()).expanduser()
            )
            udon_denominator = f"{total_udon:,}"
            brand_denominator = f"{total_brands:,}"
        except (WorkflowError, OSError, ValueError, KeyError, TypeError):
            udon_denominator = "-"
            brand_denominator = "-"
        self.search_result_vars["daily_count"].set(
            f"{summary.daily_udon:,}/{udon_denominator}玉"
        )
        self.search_result_vars["total_count"].set(
            f"{summary.total_udon:,}/{udon_denominator}玉"
        )
        self.search_result_vars["daily_brands"].set(
            f"{summary.daily_brands:,}/{brand_denominator}系列"
        )
        self.search_result_vars["total_brands"].set(
            f"{summary.match_brands:,}/{brand_denominator}系列"
        )

    def _show_answer_route(
        self, path: Path, *, source_snapshot: MapSnapshot | None = None
    ) -> None:
        if self._current_snapshot is None:
            return
        source = source_snapshot or self._current_snapshot
        self._answer_routes = trace_answer_routes(source, read_json(path))
        self._answer_route_day = self._current_snapshot.day
        self._draw_map(self._current_snapshot)

    def _draw_map(self, snapshot: MapSnapshot) -> None:
        canvas = self.map_canvas
        canvas.delete("all")
        preferred_radius = 18 if max(snapshot.width, snapshot.height) <= 16 else (
            14 if max(snapshot.width, snapshot.height) <= 24 else 11
        )
        canvas_width = max(100, canvas.winfo_width())
        canvas_height = max(100, canvas.winfo_height())
        width_factor = 2.0 + math.sqrt(3) * (snapshot.width + 0.5)
        height_factor = 2.0 + 1.5 * max(0, snapshot.height - 1)
        fitting_radius = int(min(
            (canvas_width - 28) / width_factor,
            (canvas_height - 28) / height_factor,
        ))
        radius = max(6, min(preferred_radius, fitting_radius))
        hex_width = math.sqrt(3) * radius
        margin = radius + 12
        total_width = margin * 2 + hex_width * (snapshot.width + 0.5)
        total_height = margin * 2 + 1.5 * radius * max(0, snapshot.height - 1)
        offset_x = max(0.0, (canvas_width - total_width) / 2.0)
        offset_y = max(0.0, (canvas_height - total_height) / 2.0)
        agent_groups: dict[int, list[Any]] = {}
        for agent in snapshot.agents:
            agent_groups.setdefault(agent.pos, []).append(agent)

        for y, row in enumerate(snapshot.cells):
            for x, terrain in enumerate(row):
                pos = y * snapshot.width + x
                cx = offset_x + margin + hex_width * (x + (0.5 if y % 2 == 0 else 0.0))
                cy = offset_y + margin + 1.5 * radius * y
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
                        tags=(tag, "spot-label"),
                    )
                    detail = f"pos={pos} ({x},{y}) spot brand={brand} stocks={stocks}"
                else:
                    detail = f"pos={pos} ({x},{y}) terrain={terrain}"
                canvas.tag_bind(tag, "<Button-1>", lambda _event, value=detail: self.status_var.set(value))

                for offset, agent in enumerate(agent_groups.get(pos, [])):
                    ax = cx - radius * 0.45 + offset * radius * 0.42
                    ay = cy + radius * 0.48
                    label = "P" if agent.kind == 0 else ("S" if agent.kind == 1 else "?")
                    fill = self.ROUTE_COLORS[agent.index % len(self.ROUTE_COLORS)]
                    size = max(5, radius * 0.28)
                    canvas.create_oval(ax - size, ay - size, ax + size, ay + size,
                                      fill=fill, outline="", tags=("agent-label",))
                    canvas.create_text(
                        ax,
                        ay,
                        text=f"{agent.index}{label}",
                        fill="white",
                        font=("TkDefaultFont", max(6, radius // 3), "bold"),
                        tags=("agent-label",),
                    )

        if self._answer_routes is not None and snapshot.day == self._answer_route_day:
            for agent_index, route in enumerate(self._answer_routes):
                for start, end in zip(route, route[1:]):
                    points = []
                    for pos in (start, end):
                        x, y = pos % snapshot.width, pos // snapshot.width
                        points.extend((
                            offset_x + margin + hex_width * (x + (0.5 if y % 2 == 0 else 0.0)),
                            offset_y + margin + 1.5 * radius * y,
                        ))
                    canvas.create_line(
                        *points,
                        fill=self.ROUTE_COLORS[agent_index % len(self.ROUTE_COLORS)],
                        width=max(2, radius * 0.24),
                        tags=("answer-route", f"answer-route-agent-{agent_index}"),
                    )
            canvas.tag_raise("spot-label")
            canvas.tag_raise("agent-label")

        canvas.configure(scrollregion=(
            0,
            0,
            max(canvas_width, total_width),
            max(canvas_height, total_height),
        ))

    def _map_resized(self, _event: tk.Event[Any]) -> None:
        if self._current_snapshot is None:
            return
        if self._map_redraw_job is not None:
            self.after_cancel(self._map_redraw_job)
        self._map_redraw_job = self.after_idle(self._redraw_map_after_resize)

    def _redraw_map_after_resize(self) -> None:
        self._map_redraw_job = None
        if self._current_snapshot is not None:
            self._draw_map(self._current_snapshot)

    def _fill_agents(self, snapshot: MapSnapshot) -> None:
        self.agent_table.delete(*self.agent_table.get_children())
        for agent in snapshot.agents:
            x = agent.pos % snapshot.width
            y = agent.pos // snapshot.width
            color_tag = f"agent-color-{agent.index}"
            self.agent_table.tag_configure(
                color_tag, foreground=self.ROUTE_COLORS[agent.index % len(self.ROUTE_COLORS)]
            )
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
                tags=(color_tag,),
            )

    def _fill_spots(self, snapshot: MapSnapshot) -> None:
        self.spot_table.delete(*self.spot_table.get_children())
        for pos, (brand, stocks) in sorted(snapshot.spots.items()):
            x = pos % snapshot.width
            y = pos // snapshot.width
            self.spot_table.insert(
                "",
                tk.END,
                values=(pos, f"({x},{y})", brand, stocks),
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
