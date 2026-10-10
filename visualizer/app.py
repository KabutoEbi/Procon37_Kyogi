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

import math
import queue
import threading
import time
from dataclasses import dataclass, replace
from datetime import datetime
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any, Callable

from backend import (
    PROJECT_ROOT,
    DEFAULT_TIMEOUT_SECONDS,
    AutomaticMatch,
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
    day_time_limit_ms,
    load_snapshot,
    file_hash,
    format_json_for_display,
    match_acquisition_theoretical,
    map_acquisition_totals,
    parse_solver_result,
    parse_spoken_checkpoint,
    read_json,
    simple_server_teams,
)


APP_ICON_PATH = Path(__file__).resolve().parent / "assets" / "app_icon.png"

@dataclass(frozen=True)
class TeamRoleProfile:
    label: str
    build_label: str
    time_limit_ms: int
    production_time_limit_ms: int
    udon_sa: int
    stable_beam_stop: int = 0
    sa_stagnation_stop: int = 0


TEAM_ROLE_PROFILES = {
    "lead": TeamRoleProfile(
        "担当1 停滞検出ビーム（締切10秒前まで）", "担当1・停滞検出ビーム",
        30000, 60000, 0,
    ),
    "middle": TeamRoleProfile(
        "担当2 収束ビーム（予行約15秒／本番約35秒）",
        "担当2・収束ビーム", 20000, 40000, 0, 1, 0,
    ),
    "long": TeamRoleProfile(
        "担当3 収束SA（予行約15秒／本番約45秒）",
        "担当3・収束SA", 20000, 50000, 1, 0, 1,
    ),
}
TEAM_ROLE_LABELS = {
    "normal": "通常",
    **{role: profile.label for role, profile in TEAM_ROLE_PROFILES.items()},
}


@dataclass(frozen=True)
class MatchOperationProfile:
    label: str
    selection_time_ms: int
    fallback_day_time_ms: int


MATCH_OPERATION_PROFILES = {
    "connection_test": MatchOperationProfile(
        "接続テスト（本番URL・送信のみダミー・各20秒）", 20000, 20000,
    ),
    "rehearsal": MatchOperationProfile(
        "予行演習（本番URL・実探索/実送信・各30秒）", 30000, 30000,
    ),
    "production": MatchOperationProfile(
        "第1試合（本番URL・実探索/実送信・各60秒）", 60000, 60000,
    ),
    "semifinal": MatchOperationProfile(
        "準決勝（24×24・選択90秒／回答60秒）", 90000, 60000,
    ),
    "final": MatchOperationProfile(
        "決勝（32×32・選択120秒／回答60秒）", 120000, 60000,
    ),
}


def team_role_time_limit_ms(role: str, match_profile_key: str) -> int:
    """試合区分に対応する担当別の日次探索上限を返す。"""
    profile = TEAM_ROLE_PROFILES[role]
    if role == "lead":
        return MATCH_OPERATION_PROFILES[match_profile_key].fallback_day_time_ms
    return (profile.production_time_limit_ms
            if match_profile_key in {"production", "semifinal", "final"}
            else profile.time_limit_ms)


def match_setting_issues(setting: dict[str, Any], profile_key: str) -> tuple[str, ...]:
    """取得した実サーバー設定が案内された試合条件と一致するか返す。"""
    issues: list[str] = []
    day_seconds = setting.get("daySeconds")
    if profile_key == "connection_test":
        return (() if day_seconds == [20] * 4
                else ("行動回答時間または日数",))
    map_value = setting.get("map")
    spots = setting.get("spots")
    if profile_key in {"semifinal", "final"}:
        width, agent_count, spot_count, brand_min, brand_max = (
            (24, 5, 24, 14, 20) if profile_key == "semifinal"
            else (32, 7, 32, 20, 28)
        )
        brand_count = (len({spot.get("brand") for spot in spots
                            if isinstance(spot, dict)})
                       if isinstance(spots, list) else -1)
        expected = (
            (isinstance(day_seconds, list) and bool(day_seconds)
             and all(type(seconds) is int and seconds == 60
                     for seconds in day_seconds),
             "行動回答時間"),
            (isinstance(map_value, dict) and map_value.get("width") == width
             and map_value.get("height") == width, "マップサイズ"),
            (isinstance(setting.get("agents"), list)
             and len(setting["agents"]) == agent_count, "エージェント数"),
            (isinstance(spots, list) and len(spots) == spot_count, "スポット数"),
            (brand_min <= brand_count <= brand_max, "系列数"),
        )
        return tuple(label for valid, label in expected if not valid)
    expected_day_seconds = [60] * 5 if profile_key == "production" else [30] * 5
    expected = (
        (day_seconds == expected_day_seconds, "行動回答時間または日数"),
        (setting.get("daySteps") == [32, 36, 40, 40, 48], "日別ステップ数"),
        (isinstance(map_value, dict) and map_value.get("width") == 16
         and map_value.get("height") == 16, "マップサイズ"),
        (isinstance(setting.get("agents"), list)
         and len(setting["agents"]) == 4, "エージェント数"),
        (isinstance(spots, list) and len(spots) == 16, "スポット数"),
        (isinstance(spots, list)
         and len({spot.get("brand") for spot in spots if isinstance(spot, dict)}) == 10,
         "系列数"),
        (isinstance(spots, list)
         and sum(spot.get("stocks", 0) for spot in spots if isinstance(spot, dict)) == 30,
         "最大在庫数合計"),
        (setting.get("fuelLimits") == 40, "燃料積載量上限"),
        (setting.get("busyThreshold") == 1, "混雑基準値"),
        (setting.get("jammedThreshold") == 2, "渋滞基準値"),
    )
    issues.extend(label for valid, label in expected if not valid)
    return tuple(issues)


def official_score_text(summary: SolverResultSummary) -> str:
    """口頭比較用の公式辞書順スコアを返す。"""
    return (
        f"{summary.match_brands:,} / {summary.total_daily_brands:,} / "
        f"{summary.total_udon:,}"
    )


def duration_text(seconds: float) -> str:
    """経過・残り時間を、長時間でも桁落ちしない分秒で表示する。"""
    value = max(0, int(seconds))
    minutes, secs = divmod(value, 60)
    return f"{minutes:02d}:{secs:02d}"


def automatic_phase_is_waiting(label: str) -> bool:
    """サーバー側の公開時刻を待つだけで、処理時間を計らない段階か返す。"""
    return "公開待ち" in label


def compact_clock_status(values: dict[str, str], automatic_phase: str) -> str:
    """時計上段を、幅が変わりにくい日程進捗と短い状態へまとめる。"""
    automatic_labels = (
        ("問題情報", "設定取得"),
        ("エージェント種別選択", "エージェント選択"),
        ("種別送信", "エージェント送信"),
        ("当日マップ", "状態取得"),
        ("探索", "探索中"),
        ("回答送信", "回答送信"),
        ("全日程", "完了"),
    )
    state = ""
    if automatic_phase not in ("", "未実行"):
        if automatic_phase_is_waiting(automatic_phase):
            state = "待機中"
        elif automatic_phase in ("完了", "一時停止", "停止"):
            state = automatic_phase
        else:
            state = next(
                (short for marker, short in automatic_labels
                 if marker in automatic_phase),
                "処理中",
            )
    if not state:
        phase = values["phase"]
        if "エージェント選択" in phase:
            state = "エージェント選択"
        elif "試合待機" in phase:
            state = "開始待ち"
        elif phase.endswith("日目"):
            state = "待機中"
        elif "終了" in phase:
            state = "完了"
        else:
            state = "時刻待ち"
    return f"{values['progress']} {state}"


def match_clock_values(
    starts_at: float,
    day_seconds: tuple[int, ...],
    now: float,
    selection_ends_at: float = 0.0,
    current_day: int | None = None,
    day_ends_at: float = 0.0,
) -> dict[str, str]:
    """設定の絶対時刻から、GUI用の試合経過表示を作る。"""
    total = sum(day_seconds)
    if not day_seconds:
        return {
            "phase": "時刻未確定",
            "selection": "受付期限はサーバー依存",
            "match": "-",
            "day": "-",
            "progress": "0/0日",
            "timer": "--:--",
        }

    if starts_at <= 0:
        if (current_day is not None and 0 <= current_day < len(day_seconds)
                and day_ends_at > 0):
            seconds = day_seconds[current_day]
            remaining = day_ends_at - now
            if remaining > 0:
                elapsed = max(0.0, seconds - remaining)
                day_text = (f"経過 {duration_text(elapsed)} / {duration_text(seconds)}"
                            f"（残り {duration_text(remaining)}）")
                timer = duration_text(remaining)
                phase = f"{current_day + 1}日目"
            else:
                day_text = f"終了後 {duration_text(-remaining)}"
                timer = duration_text(-remaining)
                phase = f"{current_day + 1}日目終了"
            return {
                "phase": phase,
                "selection": "終了",
                "match": f"開始時刻不明 / 予定 {duration_text(total)}",
                "day": day_text,
                "progress": f"{current_day + 1}/{len(day_seconds)}日",
                "timer": timer,
            }
        return {
            "phase": "時刻情報待ち",
            "selection": "受付期限はサーバー依存",
            "match": f"予定 {duration_text(total)}",
            "day": "-",
            "progress": f"0/{len(day_seconds)}日",
            "timer": "--:--",
        }

    if selection_ends_at > 0:
        selection = (f"残り {duration_text(selection_ends_at - now)}"
                     if now < selection_ends_at else "終了")
    elif now < starts_at:
        selection = f"試合開始まで {duration_text(starts_at - now)}"
    else:
        selection = "終了"

    elapsed = now - starts_at
    if elapsed < 0:
        if selection_ends_at > now:
            phase = "開始前（エージェント選択）"
            timer = duration_text(selection_ends_at - now)
        else:
            phase = "開始前（試合待機）"
            timer = duration_text(-elapsed)
        return {
            "phase": phase,
            "selection": selection,
            "match": f"開始まで {duration_text(-elapsed)} / 予定 {duration_text(total)}",
            "day": "-",
            "progress": f"0/{len(day_seconds)}日",
            "timer": timer,
        }
    if elapsed >= total:
        return {
            "phase": "全日程終了",
            "selection": selection,
            "match": f"経過 {duration_text(elapsed)} / 予定 {duration_text(total)}",
            "day": f"終了後 {duration_text(elapsed - total)}",
            "progress": f"{len(day_seconds)}/{len(day_seconds)}日",
            "timer": duration_text(elapsed - total),
        }

    day_start = 0
    for index, seconds in enumerate(day_seconds):
        day_end = day_start + seconds
        if elapsed < day_end:
            day_elapsed = elapsed - day_start
            return {
                "phase": f"{index + 1}日目",
                "selection": selection,
                "match": f"経過 {duration_text(elapsed)} / 予定 {duration_text(total)}",
                "day": (f"経過 {duration_text(day_elapsed)} / {duration_text(seconds)}"
                        f"（残り {duration_text(day_end - elapsed)}）"),
                "progress": f"{index + 1}/{len(day_seconds)}日",
                "timer": duration_text(day_end - elapsed),
            }
        day_start = day_end
    raise AssertionError("unreachable match clock state")


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
        self._app_icon: tk.PhotoImage | None = None
        try:
            self._app_icon = tk.PhotoImage(file=str(APP_ICON_PATH))
            self.iconphoto(True, self._app_icon)
        except (OSError, tk.TclError):
            # アイコンが読めない環境でも、操作画面そのものは利用可能にする。
            self._app_icon = None
        self.title("ビジュアライザ")
        self.geometry("1380x880")
        self.minsize(1040, 700)

        self._messages: queue.Queue[tuple[str, Any, Any]] = queue.Queue()
        self._busy = False
        self._submission_busy = False
        self._automatic_runner: AutomaticMatch | None = None
        self._automatic_workflow: Workflow | None = None
        self._prefetching_days: set[tuple] = set()
        self._prefetch_stop = threading.Event()
        self._action_buttons: list[ttk.Button] = []
        self._current_snapshot: MapSnapshot | None = None
        self._answer_routes: tuple[tuple[int, ...], ...] | None = None
        self._answer_route_day: int | None = None
        self._day_count = 0
        self._daily_inputs: dict[tuple, tuple[Path, Path | None, str]] = {}
        self._day_deadlines: dict[int, float] = {}
        self._map_redraw_job: str | None = None
        self._stop = threading.Event()
        self._closing = False
        self._active_token = ""
        self._selected_kinds: list[int] = []
        self._logged_schedule: tuple[int, tuple[int, ...]] | None = None
        self._milestones: dict[str, tuple[float, str]] = {}
        self._clock_starts_at = 0.0
        self._clock_day_seconds: tuple[int, ...] = ()
        self._clock_selection_ends_at = 0.0
        self._clock_current_day: int | None = None
        self._clock_day_ends_at = 0.0
        self._automatic_phase = "未実行"
        self._automatic_phase_started = 0.0
        self._final_answer_accepted = False
        self._simple_server = SimpleServer(PROJECT_ROOT / "runtime/simple-server")
        self._simple_teams: tuple[tuple[str, str], ...] = ()
        self.mode_var = tk.StringVar(value="manual")
        self.team_role_var = tk.StringVar(value=TEAM_ROLE_LABELS["normal"])
        self.match_profile_var = tk.StringVar(
            value=MATCH_OPERATION_PROFILES["production"].label
        )
        self.parameter_vars = {
            name: tk.StringVar(value=str(value))
            for name, value in vars(SearchParameters()).items()
        }
        self.parameter_vars["auto_beam_width"].set("1")
        self.random_setting_vars = {
            name: tk.StringVar(value=str(value))
            for name, value in vars(RandomSettingParameters()).items()
            if name != "seed"
        }

        self.env_var = tk.StringVar(value=str(PROJECT_ROOT / ".env"))
        self.url_var = tk.StringVar()
        self.token_var = tk.StringVar()
        self.timeout_var = tk.StringVar(value=f"{DEFAULT_TIMEOUT_SECONDS:g}")
        self.solver_var = tk.StringVar(value=str(PROJECT_ROOT / "main"))
        self.work_var = tk.StringVar(value=str(PROJECT_ROOT / "runtime"))
        self.shared_var = tk.StringVar()
        self.setting_var = tk.StringVar(value=str(PROJECT_ROOT / "runtime/setting.json"))
        self.state_var = tk.StringVar(value=str(PROJECT_ROOT / "runtime/state-day0.json"))
        self.score_var = tk.StringVar()
        self.spoken_checkpoint_var = tk.StringVar()
        self.spoken_result_var = tk.StringVar(value="- - -")
        # GUI表示は1始まり。API・JSON・ファイル名へ渡す直前に0始まりへ戻す。
        self.day_var = tk.IntVar(value=1)
        self.status_var = tk.StringVar(value="準備完了")
        self.match_score_var = tk.StringVar(value="- / - / -")
        self.match_clock_vars = {
            "status": tk.StringVar(value="0/0日 時刻待ち"),
            "phase": tk.StringVar(value="時刻未確定"),
            "selection": tk.StringVar(value="-"),
            "match": tk.StringVar(value="-"),
            "day": tk.StringVar(value="-"),
            "progress": tk.StringVar(value="0/0日"),
            "automatic": tk.StringVar(value="未実行"),
            "timer": tk.StringVar(value="--:--"),
        }
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
        self._match_profile_changed()
        self.day_var.trace_add("write", lambda *_: self._day_changed())
        for variable in (
            self.url_var, self.token_var, self.work_var,
            self.shared_var, self.setting_var,
        ):
            variable.trace_add("write", lambda *_: self._reset_daily_inputs())
        self._day_changed()
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
        self.simple_clear_button = ttk.Button(
            simple_row,
            text="作業フォルダをクリア",
            command=self._clear_simple_server_work,
        )
        self.simple_clear_button.grid(row=0, column=2, sticky=tk.E, padx=(8, 0))
        self._action_buttons.append(self.simple_clear_button)
        self.settings_button = ttk.Button(
            simple_row,
            text="実行設定・使用ファイル",
            command=self._show_settings_window,
        )
        self.settings_button.grid(row=0, column=3, sticky=tk.E, padx=(8, 0))

        simple = ttk.Frame(simple_row)
        self.simple_frame = simple
        simple.grid(row=0, column=1, sticky=tk.EW)
        simple.grid_remove()
        simple.columnconfigure(1, weight=1)
        ttk.Label(simple, text="設定JSON").grid(row=0, column=0, sticky=tk.E, padx=(0, 5))
        ttk.Entry(simple, textvariable=self.simple_config_var).grid(
            row=0, column=1, sticky=tk.EW
        )
        ttk.Button(
            simple, text="選択", width=3, padding=(2, 1),
            command=self._choose_simple_config,
        ).grid(
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
        self._action_buttons.append(self.simple_start_button)
        mode = ttk.Frame(self, padding=(12, 0, 12, 8))
        mode.pack(fill=tk.X)
        ttk.Label(mode, text="日次回答の送信").pack(side=tk.LEFT, padx=(0, 8))
        for value, label in (("manual", "手動"), ("auto", "探索後に自動")):
            ttk.Radiobutton(mode, text=label, variable=self.mode_var, value=value,
                            command=self._mode_changed).pack(side=tk.LEFT, padx=4)
        ttk.Label(mode, text="3人運用").pack(side=tk.LEFT, padx=(14, 4))
        self.team_role_box = ttk.Combobox(
            mode,
            textvariable=self.team_role_var,
            values=tuple(TEAM_ROLE_LABELS.values()),
            state="readonly",
            width=14 if sys.platform == "darwin" else 22,
        )
        self.team_role_box.pack(side=tk.LEFT, padx=4)
        self.team_role_box.bind("<<ComboboxSelected>>", self._team_role_changed)
        self.team_role_box.set(TEAM_ROLE_LABELS["normal"])
        ttk.Label(mode, text="試合区分").pack(side=tk.LEFT, padx=(14, 4))
        self.match_profile_box = ttk.Combobox(
            mode,
            textvariable=self.match_profile_var,
            values=tuple(profile.label for profile in MATCH_OPERATION_PROFILES.values()),
            state="readonly",
            width=16 if sys.platform == "darwin" else 27,
        )
        self.match_profile_box.pack(side=tk.LEFT, padx=4)
        self.match_profile_box.bind(
            "<<ComboboxSelected>>", self._match_profile_changed
        )
        self.stop_button = ttk.Button(mode, text="停止", command=self._request_stop, state=tk.DISABLED)
        self.stop_button.pack(side=tk.LEFT, padx=6)
        spoken = ttk.Frame(mode)
        spoken.pack(side=tk.LEFT, padx=10)
        ttk.Label(
            spoken,
            textvariable=self.spoken_result_var,
            anchor=tk.CENTER,
            font=("TkDefaultFont", 11, "bold"),
        ).pack(fill=tk.X)
        spoken_input = ttk.Frame(spoken)
        spoken_input.pack(fill=tk.X)
        self.spoken_checkpoint_entry = ttk.Entry(
            spoken_input, textvariable=self.spoken_checkpoint_var, width=25,
        )
        self.spoken_checkpoint_entry.pack(side=tk.LEFT, padx=(0, 2))
        self.spoken_checkpoint_entry.bind(
            "<Return>", lambda _event: self._import_spoken_checkpoint()
        )
        self.spoken_import_button = ttk.Button(
            spoken_input, text="取込", command=self._import_spoken_checkpoint,
        )
        self.spoken_import_button.pack(side=tk.LEFT, padx=(2, 0))
        self._action_buttons.append(self.spoken_import_button)

        clock = ttk.Frame(mode, width=210, height=52, padding=(10, 0, 0, 0))
        self.match_clock_frame = clock
        clock.pack(side=tk.RIGHT)
        clock.pack_propagate(False)
        ttk.Label(
            clock, textvariable=self.match_clock_vars["status"],
            anchor=tk.CENTER, width=24,
        ).pack(fill=tk.X)
        ttk.Label(
            clock, textvariable=self.match_clock_vars["timer"],
            anchor=tk.CENTER, font=("TkDefaultFont", 18, "bold"),
        ).pack(fill=tk.X)

        content = ttk.Panedwindow(self, orient=tk.VERTICAL)
        content.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 8))
        self.content_pane = content
        main = ttk.Panedwindow(content, orient=tk.HORIZONTAL)
        self.main_pane = main
        content.add(main, weight=4)

        control_panel = ttk.Frame(main, width=350)
        self.control_panel = control_panel
        control_panel.grid_propagate(False)
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
        self._apply_match_profile_settings()
        self._mode_changed()

    def _build_settings_window(self) -> None:
        window = tk.Toplevel(self)
        self.settings_window = window
        window.title("Procon37 実行設定・使用ファイル")
        window.geometry("1000x520")
        window.minsize(820, 440)
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
        check = ttk.Button(
            config, text="接続テスト（全日ダミー送信）",
            command=self._check_connection,
        )
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
        files.grid(row=4, column=0, columnspan=6, sticky=tk.EW, pady=(8, 0))
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
        frame = ttk.LabelFrame(parent, text="ランダム設定（本選3区分のみ）", padding=6)
        self.random_setting_frame = frame
        frame.grid(row=5, column=0, columnspan=6, sticky=tk.EW, pady=(8, 0))
        fields = [
            ("width", "幅"), ("height", "高さ"), ("agents", "エージェント"),
            ("spots", "スポット"), ("brands", "系列"), ("days", "日数"),
            ("seconds", "1日秒数（60固定）"), ("teams", "チーム"),
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
        traffic_row = ttk.Frame(frame)
        traffic_row.grid(row=3, column=0, columnspan=6, sticky=tk.W, padx=6, pady=3)
        ttk.Label(traffic_row, text="道路状態（ローカル探索）").pack(side=tk.LEFT)
        self.random_traffic_check = ttk.Checkbutton(
            traffic_row,
            text="混雑・渋滞を含める",
            variable=self.random_setting_vars["include_congestion"],
            onvalue="1",
            offvalue="0",
        )
        self.random_traffic_check.pack(side=tk.LEFT, padx=6)
        ttk.Label(
            frame,
            text="16×16: 4台・16スポット・10〜14系列 / 24×24: 5台・24スポット・14〜20系列 / 32×32: 7台・32スポット・20〜28系列",
            wraplength=780,
        ).grid(row=4, column=0, columnspan=6, sticky=tk.W, padx=6, pady=2)
        generate = ttk.Button(
            frame,
            text="ランダム生成して探索",
            command=self._generate_and_solve_local,
            style="Primary.TButton",
        )
        generate.grid(row=5, column=0, columnspan=6, sticky=tk.EW, padx=6, pady=(6, 2))
        self.random_generate_button = generate
        self._action_buttons.append(generate)

    def _build_parameters_tab(self, notebook: ttk.Notebook) -> None:
        frame = ttk.Frame(notebook, padding=8)
        notebook.add(frame, text="探索パラメータ")
        self.parameters_frame = frame
        fields = [
            ("auto_beam_width", "ビーム幅の自動選択", "1: 盤面・当日・時間上限に応じる / 0: 下の手動幅"),
            ("beam_width", "ビーム幅", "各展開で保持する状態数"),
            ("joint_beam_width", "同時行動ビーム幅", "複数台への行動割当の幅"),
            ("threads", "スレッド数", "0: 自動 / 1: 直列"),
            ("time_limit_ms", "探索時間上限 (ms)", "初期値: 対象日の試合時間。短い値へ変更可能"),
            ("safety_margin_ms", "締切余裕 (ms)", "探索・種別選択を締切より早く終了"),
            ("max_actions", "各車の行動候補数", "0: 全候補"),
            ("macro_actions", "マクロ行動", "1: 有効 / 0: 無効"),
            ("adaptive_beam", "ビーム幅自動調整", "自動選択が0のときだけ使用"),
            ("udon_sa", "玉数の焼きなまし", "担当1・2は0（ビーム系）/ 担当3は1（SA）"),
            ("stable_beam_stop", "ビーム安定時に終了", "担当2は1: 連続2段階で結果が同じなら終了"),
            ("sa_stagnation_stop", "SA停滞時に終了", "担当3は1: 全近傍試行後の無改善で終了"),
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

        add_fields(daily, fields[:10])
        add_fields(selection, fields[10:])

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
        heading = ttk.Frame(parent)
        heading.grid(row=0, column=0, sticky=tk.EW, pady=(0, 8))
        ttk.Label(heading, text="ワークフロー", style="Section.TLabel").pack(
            side=tk.LEFT
        )
        score = ttk.Frame(heading)
        self.match_score_frame = score
        score.pack(side=tk.RIGHT, padx=(8, 0))
        ttk.Label(
            score, text="系列 / 系列累計 / 総獲得数", anchor=tk.CENTER,
        ).pack(fill=tk.X)
        ttk.Label(
            score, textvariable=self.match_score_var,
            anchor=tk.CENTER, font=("TkDefaultFont", 14, "bold"),
        ).pack(fill=tk.X)

        row = 1
        self.build_solver_button = ttk.Button(
            parent,
            text="0  mainを最適化ビルド",
            command=self._build_solver,
        )
        self.build_solver_button.grid(row=row, column=0, sticky=tk.EW, pady=3)
        self._action_buttons.append(self.build_solver_button)
        row += 1

        self.prepare_button = ttk.Button(
            parent,
            text="1b 問題公開待ち・設定取得・種別選択・送信",
            command=self._prepare_for_role,
            style="Primary.TButton",
        )
        self.prepare_button.grid(row=row, column=0, sticky=tk.EW, pady=3)
        self._action_buttons.append(self.prepare_button)
        row += 1

        ttk.Separator(parent).grid(row=row, column=0, sticky=tk.EW, pady=8)
        row += 1
        day_row = ttk.Frame(parent)
        day_row.grid(row=row, column=0, sticky=tk.EW)
        ttk.Label(day_row, text="対象日 day").pack(side=tk.LEFT)
        self.day_spinbox = ttk.Spinbox(
            day_row, from_=1, to=10, textvariable=self.day_var, width=5
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
            if command == self._fetch_and_solve_day:
                self.solve_day_button = button
            if command == self._send_answer:
                self.send_answer_button = button
            row += 1

        self.automatic_match_button = ttk.Button(
            parent,
            text="試合終了まで全自動",
            command=self._run_automatic_match,
            style="Primary.TButton",
        )
        self.automatic_match_button.grid(row=row, column=0, sticky=tk.EW, pady=(9, 3))
        self._action_buttons.append(self.automatic_match_button)
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
            ("total_brands", "日別系列累計"),
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
            columns=("id", "kind", "position", "fuel", "steps"),
            show="headings",
            height=6,
        )
        for key, title, width, minwidth in [
            ("id", "ID", 25, 20),
            ("kind", "種類", 42, 32),
            ("position", "位置", 90, 65),
            ("fuel", "燃料", 60, 45),
            ("steps", "ステップ", 65, 50),
        ]:
            self.agent_table.heading(key, text=title)
            self.agent_table.column(key, width=width, minwidth=minwidth, anchor=tk.CENTER)
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
        shared_text = self.shared_var.get().strip()
        shared_dir = Path(shared_text) if shared_text else None
        if shared_dir is not None and not shared_dir.expanduser().is_dir():
            raise WorkflowError(f"共有フォルダを開けません: {shared_dir}")
        return Workflow(
            ConnectionConfig(self.url_var.get().strip(), self.token_var.get(), timeout),
            Path(self.solver_var.get()),
            Path(self.work_var.get()),
            parameters,
            self._stop,
            shared_dir,
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
                if kind == "submission_ok":
                    label, callback = context
                    self._submission_busy = False
                    if callback is not None:
                        callback(payload)
                    self._log(f"✓ {label} 完了")
                    self.status_var.set(self._automatic_phase)
                    self._mode_changed()
                    continue
                if kind == "submission_error":
                    label = context
                    self._submission_busy = False
                    self._log(f"{label}: {payload}")
                    self.status_var.set(self._automatic_phase)
                    self._mode_changed()
                    if not self._closing:
                        messagebox.showerror(label, self._redact(str(payload)), parent=self)
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
                    if label == "試合終了まで全自動":
                        self._finish_automatic_phase("完了")
                        self._automatic_runner = None
                        self._automatic_workflow = None
                    self._set_busy(False, f"{label} 完了")
                else:
                    label, callback = context
                    self._log(f"{label}: {payload}")
                    if callback is not None:
                        try:
                            callback(payload)
                        except Exception as error:
                            self._log(f"表示更新エラー: {error}")
                    if label == "試合終了まで全自動":
                        self._finish_automatic_phase(
                            "一時停止" if isinstance(payload, Paused) else "停止"
                        )
                        self._automatic_runner = None
                        self._automatic_workflow = None
                    self._set_busy(False, "停止しました" if isinstance(payload, Paused) else f"{label} 失敗")
                    if not isinstance(payload, Paused) and not self._closing:
                        messagebox.showerror(label, self._redact(str(payload)), parent=self)
        except queue.Empty:
            pass
        except Exception as error:
            self._log(f"画面更新エラー: {error}")
        self._emit_due_milestones()
        self._update_match_clock()
        self._advance_finished_day()
        if self._closing and not self._busy and not self._submission_busy:
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
        self._sync_day_button()

    def _set_controls(self, parent: tk.Widget, disabled: bool) -> None:
        for widget in parent.winfo_children():
            if isinstance(widget, (ttk.Entry, ttk.Button, ttk.Spinbox, ttk.Checkbutton)):
                widget.configure(state=tk.DISABLED if disabled else tk.NORMAL)
            self._set_controls(widget, disabled)

    def _mode_changed(self) -> None:
        role = self._team_role()
        team_mode = role != "normal"
        follower_mode = role in ("middle", "long")
        if role == "lead":
            self.mode_var.set("auto")
        elif team_mode:
            self.mode_var.set("manual")
        waiting_candidate = self._waiting_resubmission_candidate()
        self.send_answer_button.configure(
            text="3  前日の回答を再送（ベスト解）" if waiting_candidate else "3  日次回答を送信",
            state=(tk.NORMAL if waiting_candidate else
                   tk.DISABLED if self._busy or self._submission_busy
                   or self.mode_var.get() == "auto" else tk.NORMAL),
        )
        if hasattr(self, "automatic_match_button"):
            self.automatic_match_button.configure(
                text=("担当1：試合終了まで全自動" if role == "lead" else
                      "追走担当は日次ボタンを使用" if follower_mode else
                      "試合終了まで全自動"),
                state=tk.DISABLED if self._busy or follower_mode else tk.NORMAL,
            )
        if hasattr(self, "prepare_button"):
            self.prepare_button.configure(
                text=("1a 問題公開待ち・設定のみ取得（追走端末）"
                      if follower_mode else
                      "1b 問題公開待ち・設定取得・種別選択・送信")
            )
        if hasattr(self, "build_solver_button"):
            profile = TEAM_ROLE_PROFILES.get(role)
            match_label = self._match_profile().label.split("（", 1)[0]
            self.build_solver_button.configure(
                text=(f"0  mainを最適化ビルド（{match_label}・"
                      f"{profile.build_label if profile else '通常'}）")
            )
        if hasattr(self, "spoken_import_button"):
            self.spoken_import_button.configure(
                state=(tk.NORMAL if not self._busy or waiting_candidate
                       else tk.DISABLED)
            )

    def _team_role(self) -> str:
        selected = self.team_role_var.get()
        return next(
            (key for key, label in TEAM_ROLE_LABELS.items() if label == selected),
            "normal",
        )

    def _match_profile(self) -> MatchOperationProfile:
        selected = self.match_profile_var.get()
        return next(
            (profile for profile in MATCH_OPERATION_PROFILES.values()
             if profile.label == selected),
            MATCH_OPERATION_PROFILES["production"],
        )

    def _match_profile_key(self) -> str:
        selected = self.match_profile_var.get()
        return next(
            (key for key, profile in MATCH_OPERATION_PROFILES.items()
             if profile.label == selected),
            "production",
        )

    def _match_profile_changed(self, _event: object | None = None) -> None:
        self._apply_match_profile_settings()
        self._mode_changed()

    def _apply_match_profile_settings(self) -> None:
        """選択中の試合区分と担当に対応する実行設定を再適用する。"""
        profile = self._match_profile()
        self.parameter_vars["selection_time_ms"].set(
            str(profile.selection_time_ms)
        )
        self.parameter_vars["time_limit_ms"].set(
            str(profile.fallback_day_time_ms)
        )
        self._sync_day_time_limit()
        self._log(
            f"試合区分: {profile.label} / すべて.envの同一URLを使用 / "
            f"ダミーは接続テストのみ"
        )

    def _team_role_changed(self, _event: object | None = None) -> None:
        role = self._team_role()
        if role != "normal":
            self.mode_var.set("auto" if role == "lead" else "manual")
            self._apply_team_role_profile(role)
            profile = TEAM_ROLE_PROFILES[role]
            send_mode = "自動送信" if role == "lead" else "手動送信"
            self._log(
                f"3人運用: {profile.label} / {send_mode} / "
                f"time-limit-ms={team_role_time_limit_ms(role, self._match_profile_key())} / "
                f"udon-sa={profile.udon_sa} / "
                f"stable-beam-stop={profile.stable_beam_stop} / "
                f"sa-stagnation-stop={profile.sa_stagnation_stop}"
            )
        else:
            self.parameter_vars["stable_beam_stop"].set("0")
            self.parameter_vars["sa_stagnation_stop"].set("0")
            self._sync_day_time_limit()
        self._mode_changed()

    def _apply_team_role_profile(self, role: str) -> None:
        """担当別の時間・探索方式をGUI引数へ一括反映する。"""
        profile = TEAM_ROLE_PROFILES.get(role)
        if profile is None:
            return
        self.parameter_vars["auto_beam_width"].set("1")
        self.parameter_vars["time_limit_ms"].set(str(
            team_role_time_limit_ms(role, self._match_profile_key())
        ))
        self.parameter_vars["udon_sa"].set(str(profile.udon_sa))
        self.parameter_vars["stable_beam_stop"].set(
            str(profile.stable_beam_stop)
        )
        self.parameter_vars["sa_stagnation_stop"].set(
            str(profile.sa_stagnation_stop)
        )

    def _waiting_resubmission_candidate(
        self,
    ) -> tuple[Workflow, int, Path, Path, Path, Path] | None:
        """全自動が翌日待ちの間だけ、直前の受理済み候補を返す。"""
        runner = self._automatic_runner
        workflow = self._automatic_workflow
        if (not self._busy or self._submission_busy or runner is None or workflow is None
                or runner.phase != "state"
                or not automatic_phase_is_waiting(self._automatic_phase)
                or runner.day <= 0):
            return None
        day = runner.day - 1
        answer = workflow.work_dir / "answer.json"
        score = workflow.work_dir / "score-next.json"
        state = workflow.work_dir / f"state-day{day}.json"
        setting = runner.setting
        try:
            receipt = read_json(workflow.work_dir / "prepared-answer.json")
            journal = read_json(workflow.work_dir / f"submission-day{day}.json")
            if (receipt.get("day") != day or journal.get("status") != "accepted"
                    or any(not path.is_file() for path in (answer, score, state, setting))):
                return None
        except (WorkflowError, OSError, AttributeError):
            return None
        return workflow, day, answer, score, state, setting

    def _request_stop(self) -> None:
        self._stop.set()
        self.status_var.set("停止要求中… 実行中のHTTP送信は応答を確認してから停止します")

    def _close(self) -> None:
        self._closing = True
        self._prefetch_stop.set()
        self._simple_server.stop()
        if self._busy or self._submission_busy:
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
            self.day_var.set(1)
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
        self._clock_starts_at = 0.0
        self._clock_day_seconds = ()
        self._clock_selection_ends_at = 0.0
        self._clock_current_day = None
        self._clock_day_ends_at = 0.0
        self._update_match_clock()
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
        self.day_var.set(1)
        self._selected_kinds = []
        self._day_count = 0
        self._final_answer_accepted = False
        self._current_snapshot = None
        self._logged_schedule = None
        self._milestones.clear()
        self._clock_starts_at = 0.0
        self._clock_day_seconds = ()
        self._clock_selection_ends_at = 0.0
        self._clock_current_day = None
        self._clock_day_ends_at = 0.0
        self._update_match_clock()
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
        self._clock_selection_ends_at = info.selection_ends_at
        self._clock_starts_at = info.match_starts_at
        self._clock_day_seconds = tuple(info.day_seconds)
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
        # 簡易サーバーは開始前のsettingでstartsAt=0を返す。起動時に得た
        # 実時刻を未確定値で上書きしない。
        preserve_simple_server_clock = (
            starts_at <= 0 and self._clock_selection_ends_at > 0
        )
        if not preserve_simple_server_clock and self._clock_starts_at != float(starts_at):
            self._clock_selection_ends_at = 0.0
        if not preserve_simple_server_clock:
            self._clock_starts_at = float(starts_at)
        self._clock_day_seconds = day_seconds
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

    def _update_match_clock(self) -> None:
        values = match_clock_values(
            self._clock_starts_at,
            self._clock_day_seconds,
            time.time(),
            self._clock_selection_ends_at,
            self._clock_current_day,
            self._clock_day_ends_at,
        )
        for key, value in values.items():
            if self.match_clock_vars[key].get() != value:
                self.match_clock_vars[key].set(value)
        automatic = self._automatic_phase
        waiting = (
            automatic_phase_is_waiting(automatic)
            or values["phase"] == "開始前（試合待機）"
        )
        if waiting:
            values["timer"] = "--:--"
            self.match_clock_vars["timer"].set(values["timer"])
        elif values["timer"] == "--:--" and self._automatic_phase_started > 0:
            values["timer"] = duration_text(
                time.monotonic() - self._automatic_phase_started
            )
            self.match_clock_vars["timer"].set(values["timer"])
        status = compact_clock_status(values, automatic)
        if self.match_clock_vars["status"].get() != status:
            self.match_clock_vars["status"].set(status)

    def _set_automatic_phase(self, label: str) -> None:
        now = time.monotonic()
        if self._automatic_phase_started > 0 and label != self._automatic_phase:
            self._log(
                f"⏱ {self._automatic_phase}: "
                f"{duration_text(now - self._automatic_phase_started)}"
            )
        if label != self._automatic_phase:
            self._automatic_phase = label
            self._automatic_phase_started = (
                0.0 if automatic_phase_is_waiting(label) else now
            )
        self._update_match_clock()
        self._mode_changed()

    def _finish_automatic_phase(self, label: str) -> None:
        if self._automatic_phase_started > 0:
            self._log(
                f"⏱ {self._automatic_phase}: "
                f"{duration_text(time.monotonic() - self._automatic_phase_started)}"
            )
        self._automatic_phase = label
        self._automatic_phase_started = 0.0
        self._update_match_clock()

    def _reload_env(self, show_dialog: bool = True) -> None:
        try:
            config = connection_from_env(Path(self.env_var.get()))
            self.url_var.set(config.url)
            self.token_var.set(config.token)
            status = "TOKEN読込済み" if config.token else "TOKEN未設定"
            self.status_var.set(
                f".env再読込: {status}（全試合区分で共通）"
            )
        except (WorkflowError, OSError) as error:
            if show_dialog:
                messagebox.showerror(".env読込", str(error), parent=self)

    def _build_solver(self) -> None:
        role = self._team_role()
        self._apply_match_profile_settings()
        self._apply_team_role_profile(role)
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        profile = TEAM_ROLE_PROFILES.get(role)
        match_label = self._match_profile().label.split("（", 1)[0]
        label = f"{match_label}・{profile.build_label if profile else '通常'}"
        self._run_task(f"mainの最適化ビルド（{label}）", workflow.build_solver)

    @staticmethod
    def _spoken_checkpoint_text(checkpoint: dict[str, Any]) -> str:
        parsed = parse_spoken_checkpoint(
            f"{checkpoint.get('matchBrands')} {checkpoint.get('totalUdon')} "
            f"{checkpoint.get('sumOfDailyBrandCounts')}",
            checkpoint.get("nextDay"),
        )
        return (
            f"{parsed['matchBrands']} {parsed['totalUdon']} "
            f"{parsed['sumOfDailyBrandCounts']}"
        )

    def _show_spoken_checkpoint(self, checkpoint: Path) -> None:
        value = read_json(checkpoint)
        if not isinstance(value, dict):
            raise WorkflowError("次日チェックポイントの形式が不正です")
        text = self._spoken_checkpoint_text(value)
        self.spoken_result_var.set(text)
        next_day = value["nextDay"]
        self._final_answer_accepted = bool(
            self._day_count and next_day >= self._day_count
        )
        if self._final_answer_accepted:
            self.status_var.set("最終日回答受理: 3値の取込不要")
            self._log(
                "✓ 最終結果3値（次日への取込不要、系列mask 玉数 "
                f"日別系列累計）: {text}"
            )
        else:
            self.status_var.set(f"口頭共有 day={next_day + 1}: {text}")
            self._log(
                f"✓ 口頭共有3値（系列mask 玉数 日別系列累計）: {text}"
            )

    def _show_spoken_candidate(self, checkpoint: Path) -> None:
        try:
            value = read_json(checkpoint)
            if not isinstance(value, dict):
                raise WorkflowError("次日チェックポイントの形式が不正です")
            text = self._spoken_checkpoint_text(value)
            self.spoken_result_var.set(text)
            self._log(
                f"✓ 探索結果の伝達3値（系列mask 玉数 日別系列累計）: {text}"
            )
        except (WorkflowError, OSError, ValueError, tk.TclError) as error:
            self._log(f"探索結果の伝達3値を表示できません: {error}")

    def _import_spoken_checkpoint(self) -> None:
        try:
            past_last_day = bool(
                self._day_count and self.day_var.get() > self._day_count
            )
        except tk.TclError:
            past_last_day = False
        if self._final_answer_accepted or past_last_day:
            message = "最終日の回答後は次の日がないため、3値の取込は不要です。"
            self.status_var.set("全日程終了: 3値の取込不要")
            self._log(f"✓ {message}")
            messagebox.showinfo("口頭引継ぎ", message, parent=self)
            return
        waiting = self._waiting_resubmission_candidate()
        workflow = waiting[0] if waiting is not None else self._workflow_or_dialog()
        if workflow is None:
            return
        next_day = self._day_or_dialog()
        if next_day is None:
            return
        try:
            with workflow.artifact_lock:
                output = workflow.import_spoken_checkpoint(
                    self.spoken_checkpoint_var.get(), next_day,
                )
            self.spoken_checkpoint_var.set("")
            self.score_var.set(str(output))
            for context, (state, _checkpoint, digest) in list(self._daily_inputs.items()):
                if context[-1] == next_day:
                    self._daily_inputs[context] = (state, output, digest)
            self.status_var.set(
                f"口頭引継ぎ完了: day={next_day + 1}"
            )
            self._log(
                f"✓ 口頭引継ぎ day={next_day + 1}: {output}"
            )
        except (WorkflowError, OSError, ValueError, tk.TclError) as error:
            messagebox.showerror("口頭引継ぎ", str(error), parent=self)

    def _check_connection(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        if self._match_profile_key() != "connection_test":
            messagebox.showerror(
                "接続テスト",
                "ダミーデータは「接続テスト」区分でのみ送信できます。",
                parent=self,
            )
            return
        if not messagebox.askyesno(
            "接続テスト（全日ダミー送信）",
            f"{workflow.connection.url}\n\n"
            "問題・各日の状態は実サーバーから取得し、送信だけをダミーにします。\n"
            "試合前10秒、選択カウントダウン10秒、タイプ回答20秒、"
            "行動回答20秒×4日を確認します。\n"
            "ダミー種別を送信し、各日の公開を待って全ステップ待機を送信します。\n"
            "接続テスト用試合の全日程が送信対象です。\n"
            "主催者が指定した接続テスト時間中だけ実行してください。",
            parent=self,
        ):
            return
        self._run_task(
            "接続テスト（全日ダミー送信）",
            workflow.run_dummy_connection_test,
            self._log,
        )

    def _prepare_for_role(self) -> None:
        if self._team_role() in ("middle", "long"):
            self._fetch_setting_only()
        else:
            self._prepare_match()

    def _prepare_match(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        setting = self._work_path("setting.json")
        agents = self._work_path("agents.json")
        initial_agents = self._work_path("agents-initial.json")

        def prepare() -> CommandResult:
            workflow.wait_for_setting(setting)
            self._messages.put(("event", "setting", setting))
            initial = workflow.select_agents_initial(setting, initial_agents)
            self._messages.put(("event", "agents", initial_agents))
            workflow.send_agents(initial_agents, setting)
            self._messages.put((
                "event", "log", "初期エージェント種別を送信しました",
            ))
            if not workflow.selection_improvement_time_available():
                self._messages.put((
                    "event", "log",
                    "種別受付終了10秒前のため改善探索を省略しました",
                ))
                return initial
            improved = workflow.select_agents_improved(
                setting, agents, initial_agents
            )
            self._messages.put(("event", "agents", agents))
            if file_hash(agents) != file_hash(initial_agents):
                workflow.send_agents(agents, setting)
                self._messages.put((
                    "event", "log", "改善エージェント種別を送信しました",
                ))
            else:
                self._messages.put((
                    "event", "log",
                    "初期エージェント種別を上回らなかったため再送信しません",
                ))
            return improved

        self._run_task("問題取得・初期種別送信・締切10秒前まで改善", prepare)

    def _fetch_setting_only(self) -> None:
        """追走端末用。担当1が確定するエージェント種別を上書きしない。"""
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        setting = self._work_path("setting.json")

        def completed(_path: Path) -> None:
            self._setting_loaded(setting)
            self._show_json(setting)

        self._run_task(
            "問題公開待ち・設定のみ取得",
            lambda: workflow.wait_for_setting(setting),
            completed,
        )

    def _run_automatic_match(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return

        def emit(kind: str, value: Any) -> None:
            self._messages.put(("event", kind, value))

        try:
            progress = workflow.work_dir / "automatic-progress.json"
            if progress.is_file():
                runner = AutomaticMatch.restore(workflow, emit)
                if runner.setting.is_file():
                    self._setting_loaded(runner.setting)
                if runner.agents.is_file():
                    self._agents_loaded(runner.agents)
                state = getattr(runner, "state", None)
                if state is not None and state.is_file():
                    self.state_var.set(str(state))
                    self._state_loaded(state)
                if runner.checkpoint is not None:
                    self.score_var.set(str(runner.checkpoint))
            else:
                runner = AutomaticMatch(workflow, emit)
        except (WorkflowError, OSError, ValueError) as error:
            messagebox.showerror("試合終了まで全自動", str(error), parent=self)
            return
        self._automatic_runner = runner
        self._automatic_workflow = workflow
        self._run_task("試合終了まで全自動", runner.run)

    def _auto_event(self, key: str, data: Any) -> None:
        if key == "setting":
            self._setting_loaded(data)
        elif key == "agents":
            self._agents_loaded(data)
        elif key == "day_state":
            context, state, checkpoint, digest = data
            self._daily_inputs[context] = (state, checkpoint, digest)
            self.state_var.set(str(state))
            self._state_loaded(state)
            self._sync_day_button()
        elif key == "solve_start":
            self._clear_answer_route()
            self._begin_search_result(f"day={data + 1}")
        elif key == "phase":
            self._set_automatic_phase(str(data))
            self.status_var.set(str(data))
            self._log(str(data))
        elif key == "log":
            if data:
                self._log(str(data))
        elif key == "state":
            state = Path(data)
            try:
                day = read_json(state).get("day")
                if type(day) is int:
                    context = self._day_context(day)
                    self._daily_inputs[context] = (state, None, file_hash(state))
            except (WorkflowError, OSError, ValueError, AttributeError):
                pass
            self.state_var.set(str(state))
            self._state_loaded(state)
        elif key == "solved":
            self._show_search_result(data)
            if self._automatic_workflow is not None:
                self._show_spoken_candidate(
                    self._automatic_workflow.work_dir / "score-next.json"
                )
        elif key == "answer":
            self._show_answer_route(Path(data))
            self._show_json(Path(data))
        elif key == "result_state":
            self._state_loaded(Path(data))
        elif key == "accepted":
            revision, checkpoint, next_day = data
            self.score_var.set(str(checkpoint))
            self.day_var.set(next_day + 1)
            self._log(f"revision={revision}\n次日チェックポイント: {checkpoint}")
            if self._automatic_workflow is not None:
                try:
                    self._show_spoken_checkpoint(checkpoint)
                except (WorkflowError, OSError, ValueError, tk.TclError) as error:
                    self._log(f"口頭共有3値の表示に失敗しました: {error}")
        elif key == "prefetch_done":
            self._prefetching_days.discard(data)
        elif key == "prefetch_error":
            context, error = data
            self._prefetching_days.discard(context)
            if not isinstance(error, Paused):
                self._log(f"翌日状態の自動取得: {error}")

    def _day(self) -> int:
        displayed = self.day_var.get()
        maximum = self._day_count if self._day_count else 10
        if not 1 <= displayed <= maximum:
            raise WorkflowError(f"dayは1〜{maximum}で指定してください")
        return displayed - 1

    def _day_or_dialog(self) -> int | None:
        try:
            return self._day()
        except (WorkflowError, tk.TclError) as error:
            messagebox.showerror("対象日", str(error), parent=self)
            return None

    def _setting_loaded(self, path: Path) -> None:
        self._daily_inputs.clear()
        self._day_deadlines.clear()
        self._clock_current_day = None
        self._clock_day_ends_at = 0.0
        self._clear_answer_route()
        self._clear_search_result()
        self.setting_var.set(str(path))
        self.state_var.set("")
        self.score_var.set("")
        self._selected_kinds = []
        self._day_count = 0
        self._final_answer_accepted = False
        self.day_spinbox.configure(to=10)
        self.day_var.set(1)
        self._sync_day_time_limit()
        self._refresh_views()
        self._log_match_schedule(path)
        try:
            setting = read_json(path)
            issues = match_setting_issues(setting, self._match_profile_key())
            if issues:
                self._log("⚠ 試合条件不一致: " + "、".join(issues))
            else:
                self._log("✓ 選択中の試合区分と取得設定が一致しました")
        except (WorkflowError, OSError, TypeError, ValueError):
            pass

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
        end = float(starts_at)
        if starts_at > 0:
            for day, seconds in enumerate(day_seconds):
                end += seconds
                self._day_deadlines.setdefault(day, end)
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

    def _reset_daily_inputs(self) -> None:
        identity = (self.url_var.get(), self.token_var.get(),
                    self.work_var.get(), self.setting_var.get())
        if getattr(self, "_input_identity", None) == identity:
            return
        self._input_identity = identity
        self._prefetch_stop.set()
        self._prefetch_stop = threading.Event()
        self._prefetching_days.clear()
        self._daily_inputs.clear()
        self._day_deadlines.clear()
        self._logged_schedule = None
        self._milestones.clear()
        self._clock_starts_at = 0.0
        self._clock_day_seconds = ()
        self._clock_selection_ends_at = 0.0
        self._clock_current_day = None
        self._clock_day_ends_at = 0.0
        self._update_match_clock()
        self._sync_day_button()

    def _advance_finished_day(self) -> None:
        if self._busy or not self._day_count:
            return
        try:
            day = self._day()
        except (WorkflowError, tk.TclError):
            return
        original = day
        now = time.time()
        while day < self._day_count and 0 < self._day_deadlines.get(day, 0) <= now:
            day += 1
        if day != original:
            self.day_var.set(day + 1)
            self.score_var.set(str(self._work_path(f"score-day{day}.json")))
            self._log(
                f"対象日をday={day + 1}へ更新"
                if day < self._day_count else "全日程が終了しました"
            )

    def _day_context(self, day: int) -> tuple:
        setting = Path(self.setting_var.get()).expanduser().resolve()
        return (self.url_var.get().rstrip("/"), self.token_var.get(),
                str(Path(self.work_var.get()).expanduser().resolve()),
                str(setting), file_hash(setting), day)

    def _cached_day(self, context: tuple) -> tuple[Path, Path | None, str] | None:
        cached = self._daily_inputs.get(context)
        if cached is not None and (not cached[0].is_file() or file_hash(cached[0]) != cached[2]):
            self._daily_inputs.pop(context, None)
            return None
        return cached

    def _start_next_day_prefetch(self, completed_day: int) -> None:
        """3人運用では探索後すぐ、次日の状態公開を裏で待つ。"""
        if self._team_role() == "normal" or completed_day + 1 >= self._day_count:
            return
        next_day = completed_day + 1
        try:
            context = self._day_context(next_day)
            if self._cached_day(context) is not None or context in self._prefetching_days:
                return
            source = self._workflow()
        except (WorkflowError, OSError, ValueError, tk.TclError) as error:
            self._log(f"翌日状態の自動取得を開始できません: {error}")
            return
        stop = self._prefetch_stop
        workflow = Workflow(
            source.connection,
            source.solver,
            source.work_dir,
            source.parameters,
            stop,
            source.shared_dir,
        )
        state = workflow.work_dir / f"state-day{next_day}.json"
        checkpoint = workflow.work_dir / f"score-day{next_day}.json"
        self._prefetching_days.add(context)
        self._log(f"▶ day={next_day + 1} 状態の公開待ち（自動取得）")

        def worker() -> None:
            try:
                workflow.wait_for_state(next_day, state)
                if workflow.shared_dir is not None:
                    source_path, revision = workflow.import_checkpoint(
                        next_day, Path(self.setting_var.get()), checkpoint
                    )
                    self._messages.put((
                        "event", "log",
                        f"✓ 共有チェックポイント取得 revision={revision}: "
                        f"{source_path.name}",
                    ))
                digest = file_hash(state)
                self._messages.put((
                    "event", "day_state",
                    (context, state, checkpoint if checkpoint.is_file() else None, digest),
                ))
                self._messages.put((
                    "event", "log",
                    f"✓ day={next_day + 1} 状態を自動取得しました",
                ))
            except Exception as error:
                self._messages.put(("event", "prefetch_error", (context, error)))
            finally:
                self._messages.put(("event", "prefetch_done", context))

        threading.Thread(target=worker, daemon=True).start()

    def _sync_day_button(self) -> None:
        try:
            day = self._day()
            cached = self._cached_day(self._day_context(day))
        except (OSError, WorkflowError, tk.TclError, ValueError):
            cached = None
        try:
            finished = bool(self._day_count and self.day_var.get() > self._day_count)
        except (WorkflowError, tk.TclError):
            finished = False
        self.solve_day_button.configure(
            text=("全日程終了" if finished else
                  ("2  探索" if cached else "2  状態公開待ち・取得・探索")),
            state=tk.DISABLED if self._busy or finished else tk.NORMAL,
        )

    def _day_changed(self) -> None:
        self._sync_day_button()
        self._sync_day_time_limit()

    def _sync_day_time_limit(self) -> None:
        """設定済みの対象日の試合時間を探索上限の初期値にする。"""
        role = self._team_role()
        profile = TEAM_ROLE_PROFILES.get(role)
        if profile is not None:
            self.parameter_vars["time_limit_ms"].set(str(
                team_role_time_limit_ms(role, self._match_profile_key())
            ))
            return
        try:
            setting = read_json(Path(self.setting_var.get()).expanduser())
            day = self._day()
            if not isinstance(setting, dict):
                return
            self.parameter_vars["time_limit_ms"].set(
                str(day_time_limit_ms(setting, day))
            )
        except (OSError, WorkflowError, tk.TclError, ValueError):
            # 設定未取得時はSearchParametersの既定値を維持する。
            return

    def _fetch_and_solve_day(self) -> None:
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        day = self._day_or_dialog()
        if day is None:
            return
        try:
            context = self._day_context(day)
            cached = self._cached_day(context)
        except (OSError, WorkflowError) as error:
            messagebox.showerror("当日状態", str(error), parent=self)
            return
        auto_send = self.mode_var.get() == "auto"
        reserve_network_timeout = self._team_role() not in ("middle", "long")
        self._begin_search_result(f"day={day + 1}")
        setting = Path(self.setting_var.get())
        state = self._work_path(f"state-day{day}.json")
        checkpoint: Path | None = None
        if day > 0:
            score_text = (
                str(self._work_path(f"score-day{day}.json"))
                if workflow.shared_dir is not None
                else self.score_var.get().strip()
            )
            if not score_text:
                score_text = str(self._work_path(f"score-day{day}.json"))
            self.score_var.set(score_text)
            checkpoint = Path(score_text)

        if cached is not None:
            state, cached_checkpoint, _digest = cached
            if cached_checkpoint is not None:
                checkpoint = cached_checkpoint

        def fetch_and_solve() -> CommandResult:
            if cached is None:
                workflow.wait_for_state(day, state)
                if day > 0 and workflow.shared_dir is not None:
                    source_path, revision = workflow.import_checkpoint(
                        day, setting, checkpoint
                    )
                    self._messages.put((
                        "event", "log",
                        f"✓ 共有チェックポイント取得 revision={revision}: "
                        f"{source_path.name}",
                    ))
                self._messages.put(("event", "day_state",
                                    (context, state, checkpoint, file_hash(state))))
            self._messages.put(("event", "solve_start", day))
            if not auto_send:
                return workflow.solve_day(
                    setting, state, checkpoint,
                    reserve_network_timeout=reserve_network_timeout,
                )
            initial = workflow.solve_day_initial(setting, state, checkpoint)
            self._messages.put(("event", "log", initial.output))
            revision, _answer, accepted_checkpoint = workflow.send_answer(
                day, workflow.work_dir / "answer.json",
                workflow.work_dir / "score-next.json", state, setting,
            )
            self._messages.put((
                "event", "log",
                f"貪欲初期回答を送信しました (revision={revision})",
            ))
            result = initial
            daily = read_json(state)
            if workflow.improvement_time_available(daily.get("endsAt", 0)):
                stagnated = workflow.solve_day_until_stagnation(
                    setting, state, checkpoint
                )
                self._messages.put(("event", "log", stagnated.output))
                self._messages.put((
                    "event", "log",
                    ("解の変動停止を検出しました"
                     if workflow.solution_stagnated(stagnated)
                     else "回答締切10秒前まで探索しました"),
                ))
                score = workflow.work_dir / "score-next.json"
                if workflow.candidate_answer_improves(day, score):
                    revision, _answer, accepted_checkpoint = workflow.send_answer(
                        day, workflow.work_dir / "answer.json", score,
                        state, setting,
                    )
                    result = stagnated
                    self._messages.put((
                        "event", "log",
                        f"停滞時回答を送信しました (revision={revision})",
                    ))
                else:
                    workflow.restore_accepted_answer(day)
                    self._messages.put((
                        "event", "log",
                        "貪欲初期回答を上回らなかったため停滞時は再送信しません",
                    ))
            else:
                self._messages.put((
                    "event", "log",
                    "回答締切10秒前のため停滞探索を省略しました",
                ))
            if workflow.improvement_time_available(daily.get("endsAt", 0)):
                improved = workflow.solve_day_improved(
                    setting, state, checkpoint
                )
                score = workflow.work_dir / "score-next.json"
                if workflow.candidate_answer_improves(day, score):
                    revision, _answer, accepted_checkpoint = workflow.send_answer(
                        day, workflow.work_dir / "answer.json", score,
                        state, setting,
                    )
                    result = improved
                    self._messages.put((
                        "event", "log",
                        f"最終改善回答を送信しました (revision={revision})",
                    ))
                else:
                    workflow.restore_accepted_answer(day)
                    self._messages.put((
                        "event", "log",
                        "提出済みベストを上回らなかったため最終再送信はしません",
                    ))
            else:
                self._messages.put((
                    "event", "log",
                    "回答締切10秒前のため最終改善探索を省略しました",
                ))
            self._messages.put((
                "event", "accepted", (revision, accepted_checkpoint, day + 1),
            ))
            return result

        def completed(result: CommandResult) -> None:
            self.state_var.set(str(state))
            self._state_loaded(state)
            source_snapshot = self._current_snapshot
            self._show_answer_route(
                self._work_path("answer.json"), source_snapshot=source_snapshot
            )
            result_state = self._work_path("result-state.json")
            if result_state.is_file():
                self._state_loaded(result_state)
            self._show_search_result(result)
            self._show_spoken_candidate(self._work_path("score-next.json"))
            self._show_json(self._work_path("answer.json"))
            self._start_next_day_prefetch(day)

        self._run_task(
            f"day={day + 1} " + ("探索" if cached else "状態公開待ち・取得・探索"),
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
        waiting_candidate = self._waiting_resubmission_candidate()
        if waiting_candidate is not None:
            self._resubmit_waiting_answer(*waiting_candidate)
            return
        workflow = self._workflow_or_dialog()
        if workflow is None:
            return
        day = self._day_or_dialog()
        if day is None:
            return
        self._submit_answer(
            workflow, day, Path(self.state_var.get()), Path(self.setting_var.get()),
            confirm=True,
            after_accepted=lambda: self._start_next_day_prefetch(day),
        )

    def _resubmit_waiting_answer(
        self,
        workflow: Workflow,
        day: int,
        answer: Path,
        score: Path,
        state: Path,
        setting: Path,
    ) -> None:
        if not messagebox.askyesno(
            "ベスト解の再送確認",
            f"day={day + 1} の受理済み回答をもう一度送信します。\n\n{answer}\n\n"
            "3人の候補を比較し、この端末の回答が最良であることを確認してください。\n"
            "再送時刻がその日の最後の有効回答時刻になり、第4タイブレークは悪化します。",
            parent=self,
        ):
            return
        self._submission_busy = True
        self._mode_changed()
        label = f"day={day + 1} ベスト解再送"
        self.status_var.set(f"{label}中…")
        self._log(f"▶ {label}")

        def accepted(result: tuple[int, Path, Path]) -> None:
            revision, archived, checkpoint = result
            self._log(
                f"revision={revision}\n再送: {archived}\n次日チェックポイント: {checkpoint}"
            )
            try:
                self._show_spoken_checkpoint(checkpoint)
            except (WorkflowError, OSError, ValueError, tk.TclError) as error:
                self._log(f"口頭共有3値の表示に失敗しました: {error}")

        def worker() -> None:
            try:
                with workflow.artifact_lock:
                    result = workflow.send_answer(
                        day, answer, score, state, setting, force=True
                    )
                self._messages.put(("submission_ok", (label, accepted), result))
            except Exception as error:
                self._messages.put(("submission_error", label, error))

        threading.Thread(target=worker, daemon=True).start()

    def _submit_answer(
        self,
        workflow: Workflow,
        day: int,
        state: Path,
        setting: Path,
        *,
        confirm: bool,
        after_accepted: Callable[[], None] | None = None,
    ) -> None:
        answer = workflow.work_dir / "answer.json"
        score = workflow.work_dir / "score-next.json"
        if confirm and not messagebox.askyesno(
            "回答送信の確認",
            f"day={day + 1} の回答を送信します。\n\n{answer}\n\n受理後に得点チェックポイントを確定します。実行しますか？",
            parent=self,
        ):
            return

        def accepted(result: tuple[int, Path, Path]) -> None:
            revision, archived, checkpoint = result
            self._log(
                f"revision={revision}\n保存: {archived}\n次日チェックポイント: {checkpoint}"
            )
            try:
                self._show_spoken_checkpoint(checkpoint)
            except (WorkflowError, OSError, ValueError, tk.TclError) as error:
                self._log(f"口頭共有3値の表示に失敗しました: {error}")
            if after_accepted is not None:
                after_accepted()

        self._run_task(
            f"day={day + 1} 回答送信",
            lambda: workflow.send_answer(day, answer, score, state, setting),
            accepted,
        )

    def _state_loaded(self, path: Path) -> None:
        try:
            value = read_json(path)
            if isinstance(value, dict) and type(value.get("day")) is int:
                day = value["day"]
                ends_at = value.get("endsAt", 0)
                if type(ends_at) in (int, float) and ends_at > 0:
                    self._day_deadlines[day] = float(ends_at)
                    self._clock_current_day = day
                    self._clock_day_ends_at = float(ends_at)
                    self._update_match_clock()
                self.day_var.set(day + 1)
                if day > 0:
                    candidate = self._work_path(f"score-day{day}.json")
                    if candidate.is_file() or not self.score_var.get().strip():
                        self.score_var.set(str(candidate))
        except WorkflowError:
            pass
        self._refresh_views(path)

    def _refresh_views(self, state_override: Path | None = None) -> None:
        setting = Path(self.setting_var.get()).expanduser()
        if state_override is None:
            state_text = self.state_var.get().strip()
            state = Path(state_text).expanduser() if state_text else None
        else:
            state = state_override.expanduser()
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
        self.day_spinbox.configure(to=max(1, self._day_count))
        self._draw_map(snapshot)
        self._fill_agents(snapshot)
        self._fill_spots(snapshot)
        day_text = "未取得" if snapshot.day is None else str(snapshot.day + 1)
        steps = "-"
        if snapshot.day is not None and snapshot.day < len(snapshot.day_steps):
            steps = str(snapshot.day_steps[snapshot.day])
        days_text = str(len(snapshot.day_steps)) if snapshot.day_steps else "未取得"
        brand_count = len({brand for brand, _ in snapshot.spots.values()})
        stock_total = sum(snapshot.stock_limits.values())
        thresholds = ""
        try:
            setting_value = read_json(Path(self.setting_var.get()).expanduser())
            thresholds = (
                f"  busy={setting_value.get('busyThreshold', '-')}"
                f"  jammed={setting_value.get('jammedThreshold', '-')}"
            ) if isinstance(setting_value, dict) else ""
        except (WorkflowError, OSError):
            pass
        self.map_info_var.set(
            f"{snapshot.width}×{snapshot.height}  day={day_text}  days={days_text}  "
            f"steps={steps} ({'/'.join(map(str, snapshot.day_steps))})  "
            f"agents={len(snapshot.agents)}  spots={len(snapshot.spots)}  "
            f"brands={brand_count}  maxStocks={stock_total}  "
            f"fuelLimit={snapshot.fuel_limit}{thresholds}"
        )

    def _clear_answer_route(self) -> None:
        self._answer_routes = None
        self._answer_route_day = None
        self.map_canvas.delete("answer-route")

    def _clear_search_result(self) -> None:
        self.match_score_var.set("- / - / -")
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
        self.spoken_result_var.set("- - -")
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
        self.search_result_vars["target"].set(f"{prefix}day={summary.day + 1}")
        self.search_result_vars["elapsed"].set(f"{summary.elapsed_ms:,} ms")
        self.search_result_vars["expanded"].set(f"{summary.expanded:,}")
        try:
            setting_path = Path(self.setting_var.get()).expanduser()
            daily_udon, daily_brands = map_acquisition_totals(setting_path)
            patrol_count = sum(
                agent.kind == 0
                for agent in (self._current_snapshot.agents if self._current_snapshot else ())
            )
            total_udon, total_brands = match_acquisition_theoretical(
                setting_path, patrol_count
            )
            daily_udon_denominator = f"{daily_udon:,}"
            daily_brand_denominator = f"{daily_brands:,}"
            total_udon_denominator = f"{total_udon:,}"
            total_brand_denominator = f"{total_brands:,}"
        except (WorkflowError, OSError, ValueError, KeyError, TypeError):
            daily_udon_denominator = "-"
            daily_brand_denominator = "-"
            total_udon_denominator = "-"
            total_brand_denominator = "-"
        self.search_result_vars["daily_count"].set(
            f"{summary.daily_udon:,}/{daily_udon_denominator}玉"
        )
        self.search_result_vars["total_count"].set(
            f"{summary.total_udon:,}/{total_udon_denominator}玉"
        )
        self.search_result_vars["daily_brands"].set(
            f"{summary.daily_brands:,}/{daily_brand_denominator}系列"
        )
        self.search_result_vars["total_brands"].set(
            f"{summary.total_daily_brands:,}/{total_brand_denominator}系列"
        )
        self.match_score_var.set(official_score_text(summary))

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

        def cell_center(pos: int) -> tuple[float, float]:
            y, x = divmod(pos, snapshot.width)
            return (
                offset_x + margin + hex_width * (x + (0.5 if y % 2 == 0 else 0.0)),
                offset_y + margin + 1.5 * radius * y,
            )

        agent_groups: dict[int, list[Any]] = {}
        for agent in snapshot.agents:
            agent_groups.setdefault(agent.pos, []).append(agent)

        for y, row in enumerate(snapshot.cells):
            for x, terrain in enumerate(row):
                pos = y * snapshot.width + x
                cx, cy = cell_center(pos)
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
                        points.extend(cell_center(pos))
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
        step_limit = (
            snapshot.day_steps[snapshot.day]
            if snapshot.day is not None and 0 <= snapshot.day < len(snapshot.day_steps)
            else None
        )
        steps = f"{snapshot.step}/{step_limit}" if step_limit is not None else "-"
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
                    "P" if agent.kind == 0 else ("S" if agent.kind == 1 else "-"),
                    f"{agent.pos}({x},{y})",
                    f"{agent.fuel}/{snapshot.fuel_limit}",
                    steps,
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
                values=(
                    pos,
                    f"({x},{y})",
                    brand,
                    f"{stocks}/{snapshot.stock_limits.get(pos, stocks)}",
                ),
            )

    def _show_json(self, path: Path) -> None:
        try:
            value = read_json(path)
            text = format_json_for_display(value)
        except WorkflowError as error:
            messagebox.showerror("JSON表示", str(error), parent=self)
            return
        self.json_text.configure(state=tk.NORMAL)
        self.json_text.delete("1.0", tk.END)
        self.json_text.insert("1.0", text + "\n")
        self.json_text.configure(state=tk.DISABLED)
        self.status_var.set(f"表示: {path}")

    def _choose_file(
        self,
        variable: tk.StringVar | None,
        *,
        initial_dir: Path | str,
        title: str = "",
        json_file: bool = False,
        on_select: Callable[[Path], None] | None = None,
        show_json: bool = False,
    ) -> None:
        options: dict[str, Any] = {
            "parent": self,
            "initialdir": initial_dir,
            "title": title,
        }
        if json_file:
            options["filetypes"] = [("JSON", "*.json")]
        selected = filedialog.askopenfilename(**options)
        if not selected:
            return
        path = Path(selected)
        if variable is not None:
            variable.set(str(path))
        if on_select is not None:
            on_select(path)
        if show_json:
            self._show_json(path)

    def _choose_env(self) -> None:
        self._choose_file(
            self.env_var,
            initial_dir=PROJECT_ROOT,
            title=".envを選択",
            on_select=lambda _path: self._reload_env(),
        )

    def _choose_simple_config(self) -> None:
        self._choose_file(
            self.simple_config_var,
            initial_dir=DEFAULT_SIMPLE_CONFIG.parent,
            title="簡易サーバー設定JSONを選択",
            json_file=True,
            on_select=lambda _path: self._reload_simple_teams(),
        )

    def _choose_solver(self) -> None:
        self._choose_file(
            self.solver_var, initial_dir=PROJECT_ROOT, title="solverを選択"
        )

    def _choose_work_dir(self) -> None:
        path = filedialog.askdirectory(parent=self, initialdir=PROJECT_ROOT, title="作業フォルダを選択")
        if path:
            self.work_var.set(path)

    def _choose_shared_dir(self) -> None:
        initial = self.shared_var.get().strip() or str(PROJECT_ROOT)
        path = filedialog.askdirectory(
            parent=self,
            initialdir=initial,
            title="共有フォルダを選択",
        )
        if path:
            self.shared_var.set(path)

    def _choose_setting(self) -> None:
        self._choose_file(
            None,
            initial_dir=PROJECT_ROOT,
            json_file=True,
            on_select=self._setting_loaded,
            show_json=True,
        )

    def _choose_state(self) -> None:
        self._choose_file(
            self.state_var,
            initial_dir=self.work_var.get(),
            json_file=True,
            on_select=self._state_loaded,
            show_json=True,
        )

    def _choose_score(self) -> None:
        self._choose_file(
            self.score_var,
            initial_dir=self.work_var.get(),
            json_file=True,
            show_json=True,
        )


def main() -> None:
    app = SolverGUI()
    app.mainloop()


if __name__ == "__main__":
    main()
