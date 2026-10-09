"""GUIから利用する回答システム・ソルバー操作の薄いバックエンド。"""

from __future__ import annotations

import base64
import binascii
import json
import random
import secrets
import hashlib
import hmac
import math
import os
import platform
import shutil
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Callable


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TIMEOUT_SECONDS = 10.0
CHECKPOINT_CODE_PREFIX = "P37C1:"
SERVER_CLIENT_DIR = PROJECT_ROOT / "server_client"
if str(SERVER_CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_CLIENT_DIR))

from get_problem import ApiError, fetch, request_json, atomic_write  # noqa: E402
from send_answer import send  # noqa: E402


class WorkflowError(RuntimeError):
    """GUIに表示できる操作エラー。"""


class Paused(WorkflowError):
    pass


def _option_arguments(values: dict[str, int], excluded: tuple[str, ...] = ()) -> list[str]:
    arguments: list[str] = []
    for name, value in values.items():
        if name not in excluded:
            arguments.extend(["--" + name.replace("_", "-"), str(value)])
    return arguments


@dataclass(frozen=True)
class SearchParameters:
    beam_width: int = 128
    joint_beam_width: int = 512
    # Python API callers with explicit widths retain manual behavior.
    # The GUI enables automatic daily widths by default.
    auto_beam_width: int = 0
    threads: int = 0
    # GUIでは設定読込時に対象日のdaySecondsで上書きする。
    time_limit_ms: int = 60000
    safety_margin_ms: int = 5000
    max_actions: int = 4
    macro_actions: int = 1
    adaptive_beam: int = 1
    # 0は従来の改善専用ALNS、1はbrain2の焼きなましを使う。
    udon_sa: int = 1
    # 3人運用の担当2・3だけが早期終了判定を有効にする。
    stable_beam_stop: int = 0
    sa_stagnation_stop: int = 0
    # 種別選択は順位の回答時間に含まれないため、基本候補を試走する。
    trial_candidates: int = 6
    trial_beam_width: int = 32
    trial_joint_beam_width: int = 128
    # 0なら盤面別の受付終了時刻まで使う。
    selection_time_ms: int = 0
    send_reserve_ms: int = 5000
    clock_margin_ms: int = 1000

    def arguments(self) -> list[str]:
        values = vars(self)
        for name, value in values.items():
            if type(value) is not int or not 0 <= value <= 2147483647:
                raise WorkflowError(f"{name}: 0以上の整数を指定してください")
            if name in ("beam_width", "joint_beam_width",
                        "trial_beam_width", "trial_joint_beam_width") and value == 0:
                raise WorkflowError(f"{name}: 1以上を指定してください")
            if name in ("macro_actions", "adaptive_beam", "auto_beam_width",
                        "udon_sa", "stable_beam_stop",
                        "sa_stagnation_stop") and value not in (0, 1):
                raise WorkflowError(f"{name}: 0または1を指定してください")
        return _option_arguments(values, ("send_reserve_ms", "clock_margin_ms"))


@dataclass(frozen=True)
class RandomSettingParameters:
    width: int = 32
    height: int = 32
    agents: int = 7
    spots: int = 32
    brands: int = 28
    days: int = 10
    seconds: int = 60
    teams: int = 2
    seed: int = 1
    include_congestion: int = 0

    def arguments(self) -> list[str]:
        values = vars(self)
        limits = {
            "days": (4, 10), "teams": (1, 2147483647),
            "seed": (0, 18446744073709551615),
            "include_congestion": (0, 1),
        }
        for name, (lower, upper) in limits.items():
            value = values[name]
            if type(value) is not int or not lower <= value <= upper:
                raise WorkflowError(f"{name}: {lower}〜{upper}で指定してください")
        tiers = {16: (4, 16, 10, 14), 24: (5, 24, 14, 20),
                 32: (7, 32, 20, 28)}
        if type(self.width) is not int or self.width not in tiers or \
                type(self.height) is not int or self.height != self.width:
            raise WorkflowError("width/height: 16×16、24×24、32×32のみ指定できます")
        agents, spots, minimum_brands, maximum_brands = tiers[self.width]
        if type(self.agents) is not int or self.agents != agents:
            raise WorkflowError(f"agents: {self.width}×{self.width}は{agents}台です")
        if type(self.spots) is not int or self.spots != spots:
            raise WorkflowError(f"spots: {self.width}×{self.width}は{spots}個です")
        if type(self.brands) is not int or not minimum_brands <= self.brands <= maximum_brands:
            raise WorkflowError(
                f"brands: {self.width}×{self.width}は{minimum_brands}〜{maximum_brands}系列です"
            )
        if type(self.seconds) is not int or self.seconds != 60:
            raise WorkflowError("seconds: 行動回答時間は60秒です")
        return _option_arguments(values, ("include_congestion",))


@dataclass(frozen=True)
class ConnectionConfig:
    url: str
    token: str
    timeout: float = DEFAULT_TIMEOUT_SECONDS

    def validate(self) -> None:
        if not self.url.strip():
            raise WorkflowError("PROCON_URLが設定されていません")
        if not self.token.strip():
            raise WorkflowError("PROCON_TOKENが設定されていません")
        if not math.isfinite(self.timeout) or self.timeout <= 0:
            raise WorkflowError("タイムアウトは0より大きくしてください")


@dataclass(frozen=True)
class CommandResult:
    command: tuple[str, ...]
    stdout: str
    stderr: str

    @property
    def output(self) -> str:
        parts = [value.rstrip() for value in (self.stdout, self.stderr) if value.strip()]
        return "\n".join(parts)


@dataclass(frozen=True)
class SolverResultSummary:
    day: int
    stop: str
    elapsed_ms: int
    expanded: int
    daily_udon: int
    daily_brands: int
    total_udon: int
    match_brands: int
    total_daily_brands: int


def parse_solver_result(output: str) -> SolverResultSummary | None:
    """C++ソルバーのコンソール報告からGUI表示用の探索結果を取り出す。"""
    values: dict[str, str] = {}
    in_report = False
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line == "=== procon37 solver report ===":
            in_report = True
            continue
        if not in_report or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()

    required = (
        "day", "stop", "elapsed-ms", "expanded", "udon",
        "daily-brands", "total-udon", "match-brands", "total-daily-brands",
    )
    if any(key not in values for key in required):
        return None
    try:
        return SolverResultSummary(
            day=int(values["day"]),
            stop=values["stop"],
            elapsed_ms=int(values["elapsed-ms"]),
            expanded=int(values["expanded"]),
            daily_udon=int(values["udon"]),
            daily_brands=int(values["daily-brands"]),
            total_udon=int(values["total-udon"]),
            match_brands=int(values["match-brands"]),
            total_daily_brands=int(values["total-daily-brands"]),
        )
    except ValueError:
        return None


@dataclass(frozen=True)
class AgentView:
    index: int
    kind: int
    pos: int
    fuel: int


@dataclass(frozen=True)
class MapSnapshot:
    width: int
    height: int
    cells: tuple[tuple[int, ...], ...]
    spots: dict[int, tuple[int, int]]
    traffics: dict[int, int]
    agents: tuple[AgentView, ...]
    day: int | None
    day_steps: tuple[int, ...]
    fuel_limit: int
    step: int = 0
    stock_limits: dict[int, int] = field(default_factory=dict)


def trace_answer_routes(snapshot: MapSnapshot, answer: object) -> tuple[tuple[int, ...], ...]:
    """回答の移動方向を当日開始位置からたどる。待機は線に含めない。"""
    if not isinstance(answer, list) or len(answer) != len(snapshot.agents):
        raise WorkflowError("回答のエージェント数が当日状態と一致しません")
    routes = []
    for agent, actions in zip(snapshot.agents, answer):
        if not isinstance(actions, list):
            raise WorkflowError("回答の行動配列が不正です")
        pos = agent.pos
        route = [pos]
        for action in actions:
            if type(action) is not int or action > 5:
                raise WorkflowError("回答に不正な行動値があります")
            if action < 0:
                continue
            x, y = pos % snapshot.width, pos // snapshot.width
            even = y % 2 == 0
            offsets = (
                (0 if even else -1, -1),
                (1 if even else 0, -1),
                (1, 0),
                (1 if even else 0, 1),
                (0 if even else -1, 1),
                (-1, 0),
            )
            dx, dy = offsets[action]
            x, y = x + dx, y + dy
            if not 0 <= x < snapshot.width or not 0 <= y < snapshot.height:
                raise WorkflowError("回答経路がマップ外に出ます")
            pos = y * snapshot.width + x
            route.append(pos)
        routes.append(tuple(route))
    return tuple(routes)


@dataclass(frozen=True)
class SimpleServerInfo:
    url: str
    token: str
    team_label: str
    log_path: Path
    selection_ends_at: float = 0.0
    match_starts_at: float = 0.0
    day_seconds: tuple[float, ...] = ()


SIMPLE_SERVER_ROOT = (
    PROJECT_ROOT / "回答システムに関する情報・簡易版の回答用サーバー"
)
DEFAULT_SIMPLE_CONFIG = SIMPLE_SERVER_ROOT / "試合設定用JSONファイル/example.json"


def simple_server_teams(config: Path) -> tuple[tuple[str, str], ...]:
    """簡易サーバー設定から（表示名, token）を取り出す。"""
    value = read_json(config)
    if not isinstance(value, dict) or not isinstance(value.get("teams"), list):
        raise WorkflowError("簡易サーバー設定にteams配列がありません")
    result = []
    for index, team in enumerate(value["teams"]):
        if not isinstance(team, dict) or not isinstance(team.get("token"), str):
            raise WorkflowError(f"teams[{index}].tokenが不正です")
        name = team.get("name")
        label = str(name) if name else f"team-{index}"
        result.append((label, team["token"]))
    if not result:
        raise WorkflowError("簡易サーバー設定にチームがありません")
    return tuple(result)


def simple_server_binary() -> Path:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "darwin":
        suffix = "darwin-arm64" if machine in ("arm64", "aarch64") else "darwin-amd64"
    elif system == "linux" and machine in ("x86_64", "amd64"):
        suffix = "linux-amd64"
    elif system == "windows" and machine in ("x86_64", "amd64"):
        suffix = "windows-amd64.exe"
    else:
        raise WorkflowError(f"同梱簡易サーバーはこの環境に対応していません: {system}/{machine}")
    path = SIMPLE_SERVER_ROOT / "簡易サーバー" / f"procon-server-{suffix}"
    if not path.is_file():
        raise WorkflowError(f"簡易サーバー本体がありません: {path}")
    return path


class SimpleServer:
    """同梱の動作確認用サーバーをGUIの寿命に合わせて管理する。"""

    def __init__(self, work_dir: Path) -> None:
        self.work_dir = work_dir.expanduser().resolve()
        self.process: subprocess.Popen[str] | None = None
        self._log_file: Any = None
        self.log_path = self.work_dir / "simple-server.log"
        self._lock = threading.Lock()

    @property
    def running(self) -> bool:
        with self._lock:
            return self.process is not None and self.process.poll() is None

    @property
    def client_work_dir(self) -> Path:
        return self.work_dir / "client"

    def clear_client_work(self) -> int:
        """停止中にクライアント作業フォルダの中身だけを削除する。"""
        with self._lock:
            if self.process is not None and self.process.poll() is None:
                raise WorkflowError("簡易サーバーを停止してから作業フォルダをクリアしてください")
            path = self.client_work_dir
            path.mkdir(parents=True, exist_ok=True)
            entries = list(path.iterdir())
            try:
                for entry in entries:
                    if entry.is_symlink() or entry.is_file():
                        entry.unlink()
                    else:
                        shutil.rmtree(entry)
            except OSError as error:
                raise WorkflowError(f"作業フォルダをクリアできません: {error}") from error
            return len(entries)

    def start(
        self,
        config: Path,
        team_index: int = 0,
        port: int = 8080,
        kind_deadline: float = 30.0,
        match_start_delay: float = 2.0,
    ) -> SimpleServerInfo:
        config = config.expanduser().resolve()
        teams = simple_server_teams(config)
        if not 0 <= team_index < len(teams):
            raise WorkflowError("簡易サーバーのチーム番号が範囲外です")
        if not 1024 <= port <= 65535:
            raise WorkflowError("ポートは1024〜65535で指定してください")
        for label, value in (("種別締切", kind_deadline), ("試合開始待ち", match_start_delay)):
            if not math.isfinite(value) or value <= 0:
                raise WorkflowError(f"{label}は0より大きくしてください")
        config_value = read_json(config)
        problem = config_value.get("problem") if isinstance(config_value, dict) else None
        raw_day_seconds = problem.get("daySeconds") if isinstance(problem, dict) else None
        if not isinstance(raw_day_seconds, list) or not raw_day_seconds or any(
            type(seconds) not in (int, float) or not math.isfinite(seconds) or seconds <= 0
            for seconds in raw_day_seconds
        ):
            raise WorkflowError("簡易サーバー設定のproblem.daySecondsが不正です")
        day_seconds = tuple(float(seconds) for seconds in raw_day_seconds)
        with self._lock:
            if self.process is not None and self.process.poll() is None:
                raise WorkflowError("簡易サーバーは既に起動しています")
        launched_at = time.time()
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                raise WorkflowError(f"ポート{port}は既に使用されています")
        except (ConnectionRefusedError, TimeoutError, OSError):
            pass

        self.work_dir.mkdir(parents=True, exist_ok=True)
        command = [
            str(simple_server_binary()),
            "-config", str(config),
            "-addr", f"127.0.0.1:{port}",
            "-kind-deadline", f"{kind_deadline:g}s",
            "-match-start-delay", f"{match_start_delay:g}s",
        ]
        try:
            log_file = self.log_path.open("w", encoding="utf-8")
            process = subprocess.Popen(
                command,
                cwd=config.parent,
                text=True,
                stdout=log_file,
                stderr=subprocess.STDOUT,
            )
        except OSError as error:
            if "log_file" in locals():
                log_file.close()
            raise WorkflowError(f"簡易サーバーを起動できません: {error}") from error
        with self._lock:
            self.process = process
            self._log_file = log_file

        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                detail = self.log_text()
                self.stop()
                raise WorkflowError(f"簡易サーバーが起動直後に終了しました\n{detail}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    name, token = teams[team_index]
                    return SimpleServerInfo(
                        f"http://127.0.0.1:{port}", token, name, self.log_path,
                        launched_at + kind_deadline,
                        launched_at + kind_deadline + match_start_delay,
                        day_seconds,
                    )
            except (ConnectionRefusedError, TimeoutError, OSError):
                time.sleep(0.05)
        self.stop()
        raise WorkflowError("簡易サーバーの起動確認がタイムアウトしました")

    def stop(self) -> bool:
        with self._lock:
            process = self.process
            log_file = self._log_file
            self.process = None
            self._log_file = None
        was_running = process is not None and process.poll() is None
        if was_running:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if log_file is not None:
            log_file.close()
        return was_running

    def log_text(self) -> str:
        with self._lock:
            if self._log_file is not None:
                self._log_file.flush()
        try:
            return self.log_path.read_text(encoding="utf-8").rstrip()
        except OSError:
            return ""


def read_env_file(path: Path) -> dict[str, str]:
    """KEY=VALUE形式を読み、秘密値をログへ出さず辞書で返す。"""
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for line_number, raw in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise WorkflowError(f"{path}:{line_number}: KEY=VALUE形式ではありません")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or not key.replace("_", "A").isalnum() or key[0].isdigit():
            raise WorkflowError(f"{path}:{line_number}: 変数名が不正です")
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key] = value
    return values


def connection_from_env(
    path: Path, timeout: float = DEFAULT_TIMEOUT_SECONDS
) -> ConnectionConfig:
    values = read_env_file(path)
    # シェルから一時的に上書きできるよう、実環境を優先する。
    url = os.environ.get("PROCON_URL", values.get("PROCON_URL", ""))
    token = os.environ.get("PROCON_TOKEN", values.get("PROCON_TOKEN", ""))
    return ConnectionConfig(url=url, token=token, timeout=timeout)


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise WorkflowError(f"ファイルを読み込めません: {path}: {error.strerror}") from error
    except json.JSONDecodeError as error:
        raise WorkflowError(f"JSONが不正です: {path}: {error}") from error


def format_json_for_display(value: Any, indent: int = 2) -> str:
    """単純な配列やオブジェクトを1行に保ち、入れ子の単位で改行する。"""
    def compact(item: Any) -> str:
        return json.dumps(item, ensure_ascii=False, separators=(", ", ": "))

    def format_value(item: Any, level: int) -> str:
        if not isinstance(item, (dict, list)):
            return compact(item)
        if not item or all(
            not isinstance(child, (dict, list))
            for child in (item.values() if isinstance(item, dict) else item)
        ):
            return compact(item)

        prefix = " " * (indent * (level + 1))
        closing = " " * (indent * level)
        lines: list[str] = []
        if isinstance(item, dict):
            for key, child in item.items():
                rendered = format_value(child, level + 1).splitlines()
                head = f"{prefix}{compact(str(key))}: {rendered[0]}"
                lines.append("\n".join([head, *rendered[1:]]))
            return "{\n" + ",\n".join(lines) + f"\n{closing}}}"

        for child in item:
            rendered = format_value(child, level + 1).splitlines()
            lines.append("\n".join([prefix + rendered[0], *rendered[1:]]))
        return "[\n" + ",\n".join(lines) + f"\n{closing}]"

    if indent < 0:
        raise ValueError("indentは0以上で指定してください")
    return format_value(value, 0)


def load_snapshot(setting_path: Path, state_path: Path | None = None) -> MapSnapshot:
    setting = read_json(setting_path)
    if not isinstance(setting, dict) or not isinstance(setting.get("map"), dict):
        raise WorkflowError("試合設定にmapオブジェクトがありません")
    map_data = setting["map"]
    try:
        width = int(map_data["width"])
        height = int(map_data["height"])
        raw_cells = map_data["cells"]
    except (KeyError, TypeError, ValueError) as error:
        raise WorkflowError("試合設定のmap形式が不正です") from error
    if width <= 0 or height <= 0 or not isinstance(raw_cells, list):
        raise WorkflowError("マップ寸法またはcellsが不正です")
    if len(raw_cells) != height or any(
        not isinstance(row, list) or len(row) != width for row in raw_cells
    ):
        raise WorkflowError("cellsの行列サイズがwidth/heightと一致しません")
    cells = tuple(tuple(int(cell) for cell in row) for row in raw_cells)

    spots: dict[int, tuple[int, int]] = {}
    stock_limits: dict[int, int] = {}
    for spot in setting.get("spots", []):
        if isinstance(spot, dict):
            pos = int(spot["pos"])
            stocks = int(spot["stocks"])
            spots[pos] = (int(spot["brand"]), stocks)
            stock_limits[pos] = stocks

    traffics: dict[int, int] = {
        int(entry["pos"]): int(entry["status"])
        for entry in setting.get("localTraffics", [])
    }
    agents: tuple[AgentView, ...] = tuple(
        AgentView(index, -1, int(pos), int(setting.get("fuelLimits", 0)))
        for index, pos in enumerate(setting.get("agents", []))
    )
    day: int | None = None
    step = 0
    if state_path is not None and state_path.is_file():
        state = read_json(state_path)
        if not isinstance(state, dict):
            raise WorkflowError("当日状態はJSONオブジェクトではありません")
        if type(state.get("day")) is int:
            day = state["day"]
        if type(state.get("step")) is int:
            step = state["step"]
        for traffic in state.get("traffics", []):
            if isinstance(traffic, dict):
                traffics[int(traffic["pos"])] = int(traffic["status"])
        if "remainingStocks" in state:
            remaining_stocks = state["remainingStocks"]
            setting_spots = setting.get("spots", [])
            if not isinstance(remaining_stocks, list) or len(remaining_stocks) != len(setting_spots):
                raise WorkflowError("探索後状態のremainingStocksがスポット数と一致しません")
            for spot, stock in zip(setting_spots, remaining_stocks):
                if not isinstance(spot, dict) or type(stock) is not int or stock < 0:
                    raise WorkflowError("探索後状態のremainingStocksが不正です")
                pos = int(spot["pos"])
                brand, _ = spots[pos]
                spots[pos] = (brand, stock)
        parsed_agents = []
        for index, agent in enumerate(state.get("agents", [])):
            if not isinstance(agent, dict):
                continue
            parsed_agents.append(AgentView(
                index=index,
                kind=int(agent["kind"]),
                pos=int(agent["pos"]),
                fuel=int(agent["fuel"]),
            ))
        agents = tuple(parsed_agents)

    return MapSnapshot(
        width=width,
        height=height,
        cells=cells,
        spots=spots,
        traffics=traffics,
        agents=agents,
        day=day,
        day_steps=tuple(int(value) for value in setting.get("daySteps", [])),
        fuel_limit=int(setting.get("fuelLimits", 0)),
        step=step,
        stock_limits=stock_limits,
    )


def map_acquisition_totals(setting_path: Path) -> tuple[int, int]:
    """設定時点のうどん総数と、重複を除いた系列総数を返す。"""
    snapshot = load_snapshot(setting_path)
    total_udon = sum(stocks for _, stocks in snapshot.spots.values())
    total_brands = len({brand for brand, _ in snapshot.spots.values()})
    return total_udon, total_brands


def daily_acquisition_theoretical(
    setting_path: Path, day: int, patrol_count: int
) -> tuple[int, int]:
    """移動距離を無視した当日の獲得数・系列数の理論上限を返す。"""
    snapshot = load_snapshot(setting_path)
    if type(day) is not int or not 0 <= day < len(snapshot.day_steps):
        raise WorkflowError("設定から当日のステップ数を取得できません")
    if type(patrol_count) is not int or patrol_count < 0:
        raise WorkflowError("巡回車数が不正です")

    # 同じ巡回車は同じスポットから1日に一度だけ取得できる。
    stock_capacity = sum(
        min(stocks, patrol_count) for _, stocks in snapshot.spots.values()
    )
    # 1ステップで各巡回車が取得できるのは最大1玉。
    action_capacity = patrol_count * snapshot.day_steps[day]
    theoretical_udon = min(stock_capacity, action_capacity)
    available_brands = len({
        brand for brand, stocks in snapshot.spots.values() if stocks > 0
    })
    theoretical_brands = min(available_brands, theoretical_udon)
    return theoretical_udon, theoretical_brands


def match_acquisition_theoretical(
    setting_path: Path, patrol_count: int
) -> tuple[int, int]:
    """全日程の日別理論上限を合算して返す。"""
    snapshot = load_snapshot(setting_path)
    if type(patrol_count) is not int or patrol_count < 0:
        raise WorkflowError("巡回車数が不正です")
    totals = [0, 0]
    for day in range(len(snapshot.day_steps)):
        daily = daily_acquisition_theoretical(setting_path, day, patrol_count)
        totals[0] += daily[0]
        totals[1] += daily[1]
    return totals[0], totals[1]


def _day_duration(setting: dict[str, Any], day: int) -> float:
    """設定から指定日の制限時間を検証して返す。"""
    durations = setting.get("daySeconds")
    if not isinstance(durations, list) or not 0 <= day < len(durations):
        raise WorkflowError("設定から当日の試合時間を取得できません")
    duration = durations[day]
    if type(duration) not in (int, float):
        raise WorkflowError("設定のdaySecondsが不正です")
    return float(duration)


def day_time_limit_ms(setting: dict[str, Any], day: int) -> int:
    """対象日の試合時間をGUIの探索時間上限（ms）へ変換する。"""
    duration = _day_duration(setting, day)
    milliseconds = math.floor(duration * 1000)
    if milliseconds <= 0:
        raise WorkflowError("当日の試合時間が短すぎます")
    return min(2147483647, milliseconds)


def agent_selection_seconds(setting: dict[str, Any]) -> int:
    """本選ラウンドの盤面から、公式の種別選択時間を返す。"""
    map_value = setting.get("map")
    width = map_value.get("width") if isinstance(map_value, dict) else None
    if type(width) is not int:
        return 60
    if not 8 <= width <= 32:
        raise WorkflowError("種別選択時間を判定できる本選盤面ではありません")
    return 60 if width <= 16 else (90 if width <= 24 else 120)


def official_score_tuple(checkpoint: dict[str, Any]) -> tuple[int, int, int]:
    """得点チェックポイントを公式の第1〜3比較順へ変換する。"""
    keys = ("matchBrands", "sumOfDailyBrandCounts", "totalUdon")
    values = tuple(checkpoint.get(key, 0) for key in keys)
    if any(type(value) is not int or value < 0 for value in values):
        raise WorkflowError("得点チェックポイントが不正です")
    match_brands, daily_brands, total_udon = values
    return bin(int(match_brands)).count("1"), daily_brands, total_udon


def parse_spoken_checkpoint(text: str, next_day: int) -> dict[str, int]:
    """口頭共有された3整数を次日用チェックポイントへ変換する。"""
    if type(next_day) is not int or not 1 <= next_day <= 10:
        raise WorkflowError("口頭引継ぎはday=2以降の対象日を選んで実行してください")
    parts = text.split()
    if len(parts) != 3:
        raise WorkflowError(
            "口頭引継ぎは「系列mask 玉数 日別系列累計」の3整数を入力してください"
        )
    try:
        match_brands, total_udon, daily_brands = map(int, parts)
    except ValueError as error:
        raise WorkflowError("口頭引継ぎの3項目は10進整数で入力してください") from error
    if not 0 <= match_brands <= 0xFFFFFFFF:
        raise WorkflowError("系列maskは0以上4294967295以下で入力してください")
    if not 0 <= total_udon <= 2147483647:
        raise WorkflowError("玉数は0以上2147483647以下で入力してください")
    if not 0 <= daily_brands <= 2147483647:
        raise WorkflowError("日別系列累計は0以上2147483647以下で入力してください")
    return {
        "nextDay": next_day,
        "matchBrands": match_brands,
        "totalUdon": total_udon,
        "sumOfDailyBrandCounts": daily_brands,
    }


class Workflow:
    def __init__(
        self,
        connection: ConnectionConfig,
        solver: Path,
        work_dir: Path,
        parameters: SearchParameters | None = None,
        stop: threading.Event | None = None,
        shared_dir: Path | None = None,
    ) -> None:
        self.connection = connection
        self.solver = solver.expanduser().resolve()
        self.work_dir = work_dir.expanduser().resolve()
        self.work_dir.mkdir(parents=True, exist_ok=True)
        self.log_dir = self.work_dir / "logs"
        self.parameters = parameters or SearchParameters()
        self.stop = stop or threading.Event()
        self.shared_dir = (
            shared_dir.expanduser().resolve() if shared_dir is not None else None
        )
        # 全自動の翌日待機中には、GUIから前日のベスト候補を再送できる。
        # 再送と次日の探索が同じ answer/score ファイルを同時に扱わないようにする。
        self.artifact_lock = threading.RLock()

    def check_stop(self) -> None:
        if self.stop.is_set():
            raise Paused("停止しました。自動実行は同じ段階から再開できます")

    def _require_shared_dir(self) -> Path:
        if self.shared_dir is None:
            raise WorkflowError("共有フォルダが設定されていません")
        if not self.shared_dir.is_dir():
            raise WorkflowError(f"共有フォルダを開けません: {self.shared_dir}")
        return self.shared_dir

    def _checkpoint_package(
        self, checkpoint: Path, setting: Path, revision: int
    ) -> dict[str, Any]:
        value = read_json(checkpoint)
        next_day = value.get("nextDay") if isinstance(value, dict) else None
        if type(next_day) is not int or next_day <= 0:
            raise WorkflowError("共有するチェックポイントのnextDayが不正です")
        if type(revision) is not int or revision < 0:
            raise WorkflowError("共有するrevisionが不正です")
        package = {
            "version": 1,
            "setting": file_hash(setting),
            "identity": self.identity(),
            "nextDay": next_day,
            "revision": revision,
            "publishedAt": time.time(),
            "checkpoint": value,
        }
        package["signature"] = self._checkpoint_signature(package)
        return package

    def _checkpoint_signature(self, package: dict[str, Any]) -> str:
        signed = {key: value for key, value in package.items() if key != "signature"}
        payload = json.dumps(signed, separators=(",", ":"), sort_keys=True).encode()
        return hmac.new(
            self.connection.token.encode(), payload, hashlib.sha256
        ).hexdigest()

    def _validate_checkpoint_package(
        self, package: Any, setting: Path
    ) -> tuple[int, int, dict[str, Any]]:
        if not isinstance(package, dict):
            raise WorkflowError("共有コードの形式が不正です")
        revision = package.get("revision")
        next_day = package.get("nextDay")
        checkpoint = package.get("checkpoint")
        signature = package.get("signature")
        if (package.get("version") != 1
                or package.get("setting") != file_hash(setting)
                or package.get("identity") != self.identity()
                or type(next_day) is not int or next_day <= 0
                or type(revision) is not int or revision < 0
                or not isinstance(checkpoint, dict)
                or checkpoint.get("nextDay") != next_day
                or not isinstance(signature, str)
                or not hmac.compare_digest(
                    signature, self._checkpoint_signature(package)
                )):
            raise WorkflowError("共有コードが別の試合・チーム、または不正な内容です")
        return revision, next_day, checkpoint

    def export_checkpoint_code(
        self, checkpoint: Path, setting: Path, revision: int
    ) -> str:
        """受理済み得点をチャットで渡せる1行コードにする。"""
        package = self._checkpoint_package(checkpoint, setting, revision)
        raw = json.dumps(package, separators=(",", ":"), sort_keys=True).encode()
        encoded = base64.urlsafe_b64encode(raw).decode().rstrip("=")
        return CHECKPOINT_CODE_PREFIX + encoded

    def import_checkpoint_code(
        self, code: str, setting: Path
    ) -> tuple[int, int, Path]:
        """共有コードを検証し、次日用チェックポイントへ保存する。"""
        text = code.strip()
        if not text.startswith(CHECKPOINT_CODE_PREFIX):
            raise WorkflowError("共有コードはP37C1:から始まる必要があります")
        encoded = "".join(text[len(CHECKPOINT_CODE_PREFIX):].split())
        try:
            padding = "=" * ((4 - len(encoded) % 4) % 4)
            package = json.loads(base64.urlsafe_b64decode(encoded + padding))
        except (binascii.Error, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise WorkflowError("共有コードを読み取れません") from error
        revision, next_day, checkpoint = self._validate_checkpoint_package(
            package, setting
        )
        record = self.work_dir / f"checkpoint-code-day{next_day}.json"
        if record.is_file():
            previous = read_json(record)
            old_revision, _, old_checkpoint = self._validate_checkpoint_package(
                previous, setting
            )
            if revision < old_revision:
                raise WorkflowError(
                    f"古いrevision={revision}は取り込みません（現在={old_revision}）"
                )
            if revision == old_revision and checkpoint != old_checkpoint:
                raise WorkflowError("同じrevisionの共有コード内容が一致しません")
        journal = self.work_dir / f"submission-day{next_day - 1}.json"
        if journal.is_file():
            submitted = read_json(journal)
            local_revision = submitted.get("revision")
            if (submitted.get("status") == "accepted"
                    and type(local_revision) is int and local_revision > revision):
                raise WorkflowError(
                    f"ローカルのrevision={local_revision}より古いコードは取り込みません"
                )
        output = self.work_dir / f"score-day{next_day}.json"
        atomic_write(output, json.dumps(checkpoint).encode())
        atomic_write(record, json.dumps(package).encode())
        return revision, next_day, output

    def import_spoken_checkpoint(self, text: str, next_day: int) -> Path:
        """対象日からnextDayを補い、口頭共有の3値をローカルへ保存する。"""
        checkpoint = parse_spoken_checkpoint(text, next_day)
        output = self.work_dir / f"score-day{next_day}.json"
        atomic_write(output, json.dumps(checkpoint, indent=2).encode())
        return output

    def publish_checkpoint(
        self, checkpoint: Path, setting: Path, revision: int
    ) -> Path | None:
        """受理済みチェックポイントをrevision別の不変ファイルとして共有する。"""
        if self.shared_dir is None:
            return None
        directory = self._require_shared_dir()
        package = self._checkpoint_package(checkpoint, setting, revision)
        setting_digest = package["setting"]
        identity_digest = package["identity"]
        next_day = package["nextDay"]
        output = directory / (
            f"checkpoint-{setting_digest[:12]}-{identity_digest[:12]}-day{next_day}-"
            f"revision{revision}.json"
        )
        if output.is_file():
            existing = read_json(output)
            comparable_keys = (
                "version", "setting", "identity", "nextDay", "revision",
                "checkpoint",
            )
            if all(existing.get(key) == package.get(key) for key in comparable_keys):
                return output
            raise WorkflowError(
                f"共有フォルダに内容の異なる同一revisionがあります: {output.name}"
            )
        temporary = output.with_name(
            f"{output.name}.{os.getpid()}-{time.time_ns()}.tmp"
        )
        try:
            temporary.write_text(json.dumps(package), encoding="utf-8")
            temporary.replace(output)
        except OSError as error:
            temporary.unlink(missing_ok=True)
            raise WorkflowError(
                f"回答は受理されましたが共有フォルダへ保存できません: {error}"
            ) from error
        return output

    def import_checkpoint(
        self, day: int, setting: Path, output: Path
    ) -> tuple[Path, int] | None:
        """同じ試合・dayの最大revisionを共有フォルダから取り込む。"""
        if self.shared_dir is None:
            return None
        directory = self._require_shared_dir()
        setting_digest = file_hash(setting)
        identity_digest = self.identity()
        pattern = (
            f"checkpoint-{setting_digest[:12]}-{identity_digest[:12]}-"
            f"day{day}-revision*.json"
        )
        candidates: list[tuple[int, dict[str, Any], Path]] = []
        for path in directory.glob(pattern):
            try:
                package = read_json(path)
            except (WorkflowError, OSError, ValueError):
                continue
            if not isinstance(package, dict):
                continue
            revision = package.get("revision")
            checkpoint = package.get("checkpoint")
            if (package.get("version") != 1
                    or package.get("setting") != setting_digest
                    or package.get("identity") != identity_digest
                    or package.get("nextDay") != day
                    or type(revision) is not int or revision < 0
                    or not isinstance(checkpoint, dict)
                    or checkpoint.get("nextDay") != day):
                continue
            candidates.append((revision, checkpoint, path))
        if not candidates:
            raise WorkflowError(
                f"共有フォルダにday={day}の受理済みチェックポイントがありません"
            )
        revision, checkpoint, source = max(candidates, key=lambda item: item[0])
        atomic_write(output, json.dumps(checkpoint).encode())
        return source, revision

    def _require_solver(self) -> None:
        if not self.solver.is_file():
            raise WorkflowError(f"ソルバーがありません: {self.solver}")

    def _run(self, arguments: list[str], *, cwd: Path | None = None,
             ends_at: float = 0, day_seconds: float = 0,
             reserve_for_submission: bool = False,
             reserve_network_timeout: bool = True) -> CommandResult:
        self.check_stop()
        self.parameters.arguments()  # GUI以外から指定した値も検査する。
        reserve = self.parameters.send_reserve_ms
        if reserve_for_submission or (ends_at and reserve_network_timeout):
            reserve = max(
                reserve,
                math.ceil(self.connection.timeout * 1000) +
                self.parameters.clock_margin_ms + 1200,
            )
        margin = (
            max(self.parameters.safety_margin_ms, reserve)
            if ends_at or reserve_for_submission
            else self.parameters.safety_margin_ms
        )
        if day_seconds:
            if not math.isfinite(day_seconds) or day_seconds <= 0:
                raise WorkflowError("1日の試合時間が不正です")
            # 終了処理にも必ず時間を残し、探索だけで1日分を使い切らない。
            margin = max(margin, 1000)
        now = time.time()
        remaining = None
        if ends_at:
            remaining = ends_at - now - reserve / 1000
            if remaining <= 0:
                raise WorkflowError("送信時間を確保できないため探索を開始しません")
        extra = margin - self.parameters.safety_margin_ms
        parameters = replace(
            self.parameters, safety_margin_ms=margin,
            time_limit_ms=min(2147483647, self.parameters.time_limit_ms + extra)
                if self.parameters.time_limit_ms else 0,
            selection_time_ms=min(2147483647, self.parameters.selection_time_ms + extra)
                if self.parameters.selection_time_ms else 0,
        )
        if reserve_for_submission and ends_at:
            # 種別選択の相対上限を0（無制限）にしても、設定取得時に
            # 記録した公式受付時間を越えない。C++側はここからmarginを
            # 差し引くため、残りを最大限探索しつつPOST時間を確保する。
            selection_limit_ms = min(
                2147483647,
                max(1, math.floor((ends_at - now) * 1000)),
            )
            parameters = replace(
                parameters,
                selection_time_ms=min(
                    parameters.selection_time_ms or selection_limit_ms,
                    selection_limit_ms,
                ),
            )
        if day_seconds:
            day_limit_ms = min(2147483647, math.floor(day_seconds * 1000))
            if day_limit_ms <= margin:
                raise WorkflowError("1日の試合時間内に探索終了の余裕を確保できません")
            parameters = replace(
                parameters,
                time_limit_ms=min(
                    parameters.time_limit_ms or day_limit_ms,
                    day_limit_ms,
                ),
            )
        cutoff = None
        if ends_at:
            assert remaining is not None
            cutoff = time.monotonic() + remaining + 1  # 検証・ファイル保存用の1秒
        command = [str(self.solver), *arguments, *parameters.arguments()]
        try:
            process = subprocess.Popen(
                command,
                cwd=cwd or self.work_dir,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            while True:
                try:
                    stdout, stderr = process.communicate(timeout=0.1)
                    break
                except subprocess.TimeoutExpired:
                    expired = cutoff is not None and time.monotonic() >= cutoff
                    if self.stop.is_set() or expired:
                        process.terminate()
                        try:
                            process.communicate(timeout=2)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.communicate()
                        if expired:
                            raise WorkflowError("送信時間確保のためソルバーを終了しました。未完成回答は送信しません")
                        raise Paused("探索を停止しました")
        except OSError as error:
            raise WorkflowError(f"ソルバーを起動できません: {error}") from error
        result = CommandResult(tuple(command), stdout, stderr)
        if process.returncode != 0:
            detail = result.output or f"終了コード {process.returncode}"
            raise WorkflowError(f"ソルバーが失敗しました\n{detail}")
        return result

    def check_connection(self) -> str:
        self.connection.validate()
        try:
            value, _, status = request_json(
                self.connection.url, self.connection.token, "/setting",
                timeout=self.connection.timeout,
            )
        except ApiError as error:
            if error.status == 403:
                return "サーバーへ到達しました (403: 公開前または権限不足)。認証成功は未確認です"
            raise WorkflowError(str(error)) from error
        if status != 200 or not isinstance(value, dict) or "map" not in value:
            raise WorkflowError("接続先は試合設定を返しませんでした")
        return "通信確認成功 (GET /setting: HTTP 200)"

    def build_solver(self) -> CommandResult:
        output = self.solver
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [
            os.environ.get("CXX", "c++"),
            "-std=c++17",
            "-O3",
            "-DNDEBUG",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-pthread",
            "-Iinc",
            str(PROJECT_ROOT / "main.cpp"),
            "-o",
            str(output),
        ]
        completed = subprocess.run(
            command,
            cwd=PROJECT_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        result = CommandResult(tuple(command), completed.stdout, completed.stderr)
        if completed.returncode != 0:
            raise WorkflowError(f"ビルドに失敗しました\n{result.output}")
        return result

    def generate_random_setting(
        self,
        output: Path,
        parameters: RandomSettingParameters,
    ) -> CommandResult:
        self.check_stop()
        self._require_solver()
        output = output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + ".generate")
        temporary.unlink(missing_ok=True)
        # GUI generation is intentionally fresh on every run. Keep --seed in
        # the recorded command so a generated map can still be reproduced via CLI.
        generated_seed = secrets.randbits(64)
        command = [
            str(self.solver), "generate", str(temporary),
            *replace(parameters, seed=generated_seed).arguments(),
        ]
        try:
            completed = subprocess.run(
                command,
                cwd=self.work_dir,
                text=True,
                capture_output=True,
                check=False,
            )
        except OSError as error:
            raise WorkflowError(f"ランダム設定を生成できません: {error}") from error
        result = CommandResult(tuple(command), completed.stdout, completed.stderr)
        if completed.returncode != 0:
            temporary.unlink(missing_ok=True)
            detail = result.output or f"終了コード {completed.returncode}"
            raise WorkflowError(f"ランダム設定の生成に失敗しました\n{detail}")
        try:
            load_snapshot(temporary)
            value = read_json(temporary)
            statuses = [0, 1, 2] if parameters.include_congestion else [0]
            rng = random.Random(generated_seed)
            width = value["map"]["width"]
            value["localTraffics"] = [
                {"pos": y * width + x, "status": rng.choice(statuses)}
                for y, row in enumerate(value["map"]["cells"])
                for x, terrain in enumerate(row) if terrain == 1
            ]
            atomic_write(temporary, json.dumps(value).encode())
            temporary.replace(output)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
        return result

    def fetch_setting(self, output: Path) -> Path:
        self.connection.validate()
        try:
            value = fetch(
                self.connection.url,
                self.connection.token,
                "setting",
                output,
                timeout=self.connection.timeout,
                log_dir=self.log_dir,
            )
        except (ApiError, OSError, ValueError) as error:
            raise WorkflowError(str(error)) from error
        setting_hash = file_hash(output)
        timing_path = self.work_dir / "setting-received.json"
        timing = read_json(timing_path) if timing_path.is_file() else {}
        if timing.get("setting") != setting_hash:
            received_at = time.time()
            selection_ends_at = received_at + agent_selection_seconds(value)
            starts_at = value.get("startsAt", 0)
            if type(starts_at) in (int, float) and starts_at > received_at:
                selection_ends_at = min(selection_ends_at, float(starts_at))
            atomic_write(
                timing_path,
                json.dumps({
                    "setting": setting_hash,
                    "receivedAt": received_at,
                    "selectionEndsAt": selection_ends_at,
                }).encode(),
            )
        return output

    def wait_for_setting(
        self,
        output: Path,
        *,
        interval: float = 1.0,
        wait_seconds: float | None = None,
    ) -> Path:
        """問題公開前は再試行し、公開された設定だけを確定保存する。"""
        if not math.isfinite(interval) or interval < 0.2:
            raise WorkflowError("取得間隔は0.2秒以上にしてください")
        if wait_seconds is not None and (
            not math.isfinite(wait_seconds) or wait_seconds <= 0
        ):
            raise WorkflowError("待機上限は0より大きくしてください")
        deadline = (
            time.monotonic() + wait_seconds
            if wait_seconds is not None else None
        )
        while True:
            self.check_stop()
            try:
                return self.fetch_setting(output)
            except WorkflowError as error:
                cause = error.__cause__
                if not isinstance(cause, ApiError) or cause.status not in (0, 403):
                    raise
            if deadline is not None and time.monotonic() >= deadline:
                raise WorkflowError("settingの公開待ちがタイムアウトしました")
            self.stop.wait(interval)

    def fetch_state(self, day: int, output: Path) -> Path:
        self.connection.validate()
        downloaded = output.with_name(output.name + ".download")
        try:
            value = fetch(
                self.connection.url,
                self.connection.token,
                "state",
                downloaded,
                timeout=self.connection.timeout,
                log_dir=self.log_dir,
            )
        except (ApiError, OSError, ValueError) as error:
            raise WorkflowError(str(error)) from error
        actual_day = value.get("day")
        if actual_day != day:
            downloaded.unlink(missing_ok=True)
            displayed_day = actual_day + 1 if type(actual_day) is int else actual_day
            raise WorkflowError(
                f"取得した状態はday={displayed_day}です（期待値: {day + 1}）"
            )
        downloaded.replace(output)
        return output

    def wait_for_state(
        self,
        day: int,
        output: Path,
        *,
        interval: float = 1.0,
        wait_seconds: float = 3600,
    ) -> Path:
        """前日の状態が返る間は待ち、指定日の状態だけを確定保存する。"""
        if not math.isfinite(interval) or interval < 0.2:
            raise WorkflowError("取得間隔は0.2秒以上にしてください")
        if not math.isfinite(wait_seconds) or wait_seconds <= 0:
            raise WorkflowError("待機上限は0より大きくしてください")
        deadline = time.monotonic() + wait_seconds
        while True:
            self.check_stop()
            downloaded = output.with_name(output.name + ".poll")
            try:
                self.connection.validate()
                daily = fetch(
                    self.connection.url,
                    self.connection.token,
                    "state",
                    downloaded,
                    timeout=self.connection.timeout,
                    log_dir=self.log_dir,
                )
                actual = daily.get("day")
                if type(actual) is not int:
                    raise WorkflowError("当日状態のdayが不正です")
                if actual > day:
                    raise WorkflowError(
                        f"サーバーはday={actual + 1}です。"
                        f"期待したday={day + 1}を過ぎました"
                    )
                if actual == day:
                    downloaded.replace(output)
                    return output
                downloaded.unlink(missing_ok=True)
            except (WorkflowError, ApiError) as error:
                cause = error if isinstance(error, ApiError) else error.__cause__
                if not isinstance(cause, ApiError) or cause.status not in (0, 403):
                    raise
            if time.monotonic() >= deadline:
                raise WorkflowError("stateの公開待ちがタイムアウトしました")
            self.stop.wait(interval)

    def select_agents(self, setting: Path, output: Path) -> CommandResult:
        self._require_no_pending("agents")
        marker = self.work_dir / "selected-agents.json"
        marker.unlink(missing_ok=True)
        self._require_solver()
        timing_path = self.work_dir / "setting-received.json"
        if timing_path.is_file():
            timing = read_json(timing_path)
            if timing.get("setting") != file_hash(setting):
                raise WorkflowError("設定取得時刻の記録が現在の設定と一致しません")
        else:
            setting_value = read_json(setting)
            received_at = time.time()
            selection_ends_at = received_at + agent_selection_seconds(setting_value)
            starts_at = setting_value.get("startsAt", 0)
            if type(starts_at) in (int, float) and starts_at > received_at:
                selection_ends_at = min(selection_ends_at, float(starts_at))
            timing = {"selectionEndsAt": selection_ends_at}
        selection_ends_at = timing.get("selectionEndsAt")
        if type(selection_ends_at) not in (int, float) or selection_ends_at <= 0:
            raise WorkflowError("種別受付終了時刻の記録が不正です")
        result = self._run(
            ["select", str(setting.resolve()), str(output.resolve())],
            ends_at=float(selection_ends_at),
            reserve_for_submission=True,
        )
        atomic_write(marker, json.dumps({
            "setting": file_hash(setting), "agents": file_hash(output),
            "identity": self.identity(), "selectionEndsAt": selection_ends_at,
        }).encode())
        return result

    def solve_day(
        self,
        setting: Path,
        state: Path,
        checkpoint: Path | None = None,
        *,
        reserve_network_timeout: bool = True,
    ) -> CommandResult:
        daily = read_json(state)
        day = daily["day"]
        self._require_no_pending(f"day{day}")
        if day > 0:
            self._require_no_pending(f"day{day - 1}")
        self._invalidate_answer()
        self._require_solver()
        setting_value = read_json(setting)
        duration = _day_duration(setting_value, day)
        if day > 0 and checkpoint is None:
            raise WorkflowError("day 2以降は得点チェックポイントが必要です")
        if checkpoint is not None and read_json(checkpoint).get("nextDay") != day:
            raise WorkflowError("チェックポイントの日番号が当日状態と一致しません")
        arguments = ["solve", str(setting.resolve()), str(state.resolve())]
        if checkpoint is not None:
            arguments.append(str(checkpoint.resolve()))
        result = self._run(
            arguments,
            ends_at=daily.get("endsAt", 0),
            day_seconds=duration,
            reserve_for_submission=reserve_network_timeout,
            reserve_network_timeout=reserve_network_timeout,
        )
        answer = self.work_dir / "answer.json"
        score = self.work_dir / "score-next.json"
        receipt = {
            "day": day, "endsAt": daily.get("endsAt", 0),
            "answer": file_hash(answer), "score": file_hash(score),
            "state": file_hash(state), "setting": file_hash(setting),
            "identity": self.identity(),
        }
        atomic_write(self.work_dir / "prepared-answer.json", json.dumps(receipt).encode())
        return result

    def identity(self) -> str:
        return hashlib.sha256(
            (self.connection.url.rstrip("/") + "\n" + self.connection.token).encode()
        ).hexdigest()

    def _invalidate_answer(self) -> None:
        (self.work_dir / "prepared-answer.json").unlink(missing_ok=True)

    def _require_no_pending(self, name: str) -> None:
        path = self.work_dir / f"submission-{name}.json"
        if path.exists() and read_json(path).get("status") == "pending":
            raise WorkflowError("送信結果不明の候補を保護しています。停止位置から再開してください")

    def _submission(self, name: str, receipt: dict, payload: Path,
                    score: Path | None = None) -> tuple[Path, dict]:
        """送信前に不変の候補を保存。応答不明の別回答への置換は禁止。"""
        path = self.work_dir / f"submission-{name}.json"
        suffix = ""
        if path.exists():
            saved = read_json(path)
            if saved["receipt"] == receipt:
                return path, saved
            previous = saved["receipt"]
            # 受理済み回答だけを同じ日の新候補で更新する。結果不明や別試合は保護。
            context_keys = ("day", "endsAt", "identity", "setting", "state")
            if (score is None or saved.get("status") != "accepted"
                    or any(previous.get(key) != receipt.get(key) for key in context_keys)):
                raise WorkflowError("別の送信記録があります。復旧では同じ候補だけを使用してください。別試合は新しい作業フォルダを使用してください")
            digest = hashlib.sha256(json.dumps(receipt, sort_keys=True).encode()).hexdigest()
            suffix = "-" + digest
            history = self.work_dir / f"submission-{name}-accepted-{saved['revision']}.json"
            atomic_write(history, json.dumps(saved).encode())
        # 新候補は別ファイルへ保存し、最後に記録を切り替える。
        # 保存途中で停止しても以前の受理記録と候補の整合性を保つ。
        payload_path = self.work_dir / f"submitted-{name}{suffix}.json"
        score_path = self.work_dir / f"submitted-{name}{suffix}-score.json"
        atomic_write(payload_path, payload.read_bytes())
        if score is not None:
            atomic_write(score_path, score.read_bytes())
        saved = {"receipt": receipt, "status": "pending", "revision": None,
                 "snapshotSuffix": suffix, "createdAt": time.time()}
        atomic_write(path, json.dumps(saved).encode())
        return path, saved

    def _submission_deadline(self, ends_at: float) -> float | None:
        if not ends_at:
            return None
        deadline = ends_at - self.parameters.clock_margin_ms / 1000
        if time.time() >= deadline:
            raise WorkflowError("送信締切（時刻余裕込み）を過ぎています。受理結果不明なら得点は確定しません")
        return deadline

    def solve_local(self, setting: Path) -> CommandResult:
        """当日状態がないサンプル設定を初期状態から探索する。"""
        self._invalidate_answer()
        result_state = self.work_dir / "result-state.json"
        result_state.unlink(missing_ok=True)
        self._require_solver()
        setting_value = read_json(setting)
        duration = _day_duration(setting_value, 0)
        arguments = [str(setting.resolve())]
        if "localTraffics" in setting_value:
            kinds_path = self.work_dir / "local-agents.json"
            self._run(["select", str(setting.resolve()), str(kinds_path)])
            kinds = read_json(kinds_path)
            agents = [
                {"kind": kind, "pos": pos, "fuel": setting_value["fuelLimits"]}
                for kind, pos in zip(kinds, setting_value["agents"])
            ]
            initial = {
                "day": 0, "endsAt": 0, "agents": agents,
                "others": [{"id": i, "agents": agents}
                           for i in range(setting_value["players"] - 1)],
                "traffics": setting_value["localTraffics"],
            }
            state_path = self.work_dir / "local-state.json"
            atomic_write(state_path, json.dumps(initial).encode())
            arguments.append(str(state_path))
        result = self._run(arguments, day_seconds=duration)
        if not result_state.is_file():
            raise WorkflowError("ソルバーが探索後状態 result-state.json を生成しませんでした")
        if "localTraffics" in setting_value:
            final = read_json(result_state)
            final["traffics"] = setting_value["localTraffics"]
            atomic_write(result_state, json.dumps(final).encode())
        return result

    def send_agents(self, payload: Path, setting: Path | None = None) -> None:
        self.connection.validate()
        receipt = read_json(self.work_dir / "selected-agents.json")
        if (receipt.get("identity") != self.identity() or receipt.get("agents") != file_hash(payload)
                or (setting is not None and receipt.get("setting") != file_hash(setting))):
            raise WorkflowError("設定・接続先・種別が選択時と一致しません。種別選択を再実行してください")
        self.check_stop()
        journal, saved = self._submission("agents", receipt, payload)
        if saved["status"] == "accepted":
            return
        deadline = self._submission_deadline(receipt.get("selectionEndsAt", 0))
        attempted_at = time.time()
        saved.setdefault("attemptedAt", attempted_at)
        saved["lastAttemptedAt"] = attempted_at
        atomic_write(journal, json.dumps(saved).encode())
        try:
            send(
                self.connection.url,
                self.connection.token,
                "agent",
                payload,
                timeout=self.connection.timeout,
                log_dir=self.log_dir,
                deadline=deadline,
            )
        except (ApiError, OSError, ValueError, json.JSONDecodeError) as error:
            raise WorkflowError(str(error)) from error
        saved.update(status="accepted", acceptedAt=time.time())
        atomic_write(journal, json.dumps(saved).encode())

    def send_answer(
        self,
        day: int,
        answer: Path,
        candidate_score: Path,
        state: Path | None = None,
        setting: Path | None = None,
        *,
        force: bool = False,
    ) -> tuple[int, Path, Path]:
        self.connection.validate()
        receipt = read_json(self.work_dir / "prepared-answer.json")
        if (receipt.get("day") != day or receipt.get("identity") != self.identity()
                or receipt.get("answer") != file_hash(answer)
                or receipt.get("score") != file_hash(candidate_score)):
            raise WorkflowError("日番号・接続先・回答が探索時と一致しません。再探索してください")
        for key, path in (("state", state), ("setting", setting)):
            if path is not None and receipt.get(key) != file_hash(path):
                raise WorkflowError(f"{key}が探索時と違います。再探索してください")
        if read_json(candidate_score).get("nextDay") != day + 1:
            raise WorkflowError("得点候補の日番号が一致しません")
        self.check_stop()
        archived_answer = self.work_dir / f"answer-day{day}.json"
        existing_path = self.work_dir / f"submission-day{day}.json"
        if existing_path.is_file():
            existing = read_json(existing_path)
            previous = existing.get("receipt", {})
            context_keys = ("day", "endsAt", "identity", "setting", "state")
            same_context = all(
                previous.get(key) == receipt.get(key) for key in context_keys
            )
            if (same_context and existing.get("status") == "accepted"
                    and previous != receipt):
                suffix = existing.get("snapshotSuffix", "")
                previous_score = self.work_dir / f"submitted-day{day}{suffix}-score.json"
                if not previous_score.is_file():
                    raise WorkflowError(
                        "受理済み回答の得点記録がないため、新候補の改善を確認できません"
                    )
                if (official_score_tuple(read_json(candidate_score)) <=
                        official_score_tuple(read_json(previous_score))):
                    raise WorkflowError(
                        "受理済み回答より公式スコアが高くないため送信しません。"
                        "同点再送は回答時間のタイブレークを悪化させます"
                    )
        journal, saved = self._submission(f"day{day}", receipt, answer, candidate_score)
        suffix = saved.get("snapshotSuffix", "")
        frozen_answer = self.work_dir / f"submitted-day{day}{suffix}.json"
        frozen_score = self.work_dir / f"submitted-day{day}{suffix}-score.json"
        if file_hash(frozen_answer) != receipt["answer"] or file_hash(frozen_score) != receipt["score"]:
            raise WorkflowError("保存済み送信候補が変更されています")
        atomic_write(archived_answer, frozen_answer.read_bytes())
        checkpoint = self.work_dir / f"score-day{day + 1}.json"
        was_accepted = saved["status"] == "accepted"
        if was_accepted and not force:
            atomic_write(checkpoint, frozen_score.read_bytes())
            if setting is not None:
                self.publish_checkpoint(checkpoint, setting, saved["revision"])
            return saved["revision"], archived_answer, checkpoint
        deadline = self._submission_deadline(receipt.get("endsAt", 0))
        # 再送は明示的な再開時だけ。同じ日でなければ結果不明のまま停止。
        if saved.get("attempted") and not was_accepted:
            current = self.work_dir / "recovery-state.json"
            self.fetch_state(day, current)
            if state is None or read_json(current) != read_json(state):
                raise WorkflowError("当日状態が変わっています。結果不明の回答は再送しません")
        saved["attempted"] = True
        attempted_at = time.time()
        saved.setdefault("attemptedAt", attempted_at)
        saved["lastAttemptedAt"] = attempted_at
        atomic_write(journal, json.dumps(saved).encode())
        try:
            revision = send(
                self.connection.url,
                self.connection.token,
                "action",
                archived_answer,
                timeout=self.connection.timeout,
                log_dir=self.log_dir,
                deadline=deadline,
            )
        except (ApiError, OSError, ValueError, json.JSONDecodeError) as error:
            raise WorkflowError(str(error)) from error
        if revision is None:
            raise WorkflowError("回答送信でrevisionが返りませんでした")
        saved.update(status="accepted", revision=revision, acceptedAt=time.time())
        atomic_write(journal, json.dumps(saved).encode())
        atomic_write(checkpoint, frozen_score.read_bytes())
        if setting is not None:
            self.publish_checkpoint(checkpoint, setting, revision)
        return revision, archived_answer, checkpoint


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class AutomaticMatch:
    """停止時は段階を保持する。POSTに失敗した場合は自動再送しない。"""

    def __init__(self, workflow: Workflow, emit: Callable[[str, Any], None],
                 start_day: int | None = None, checkpoint: Path | None = None,
                 interval: float = 1.0, wait_seconds: float = 3600) -> None:
        if not math.isfinite(interval) or interval < 0.2:
            raise WorkflowError("取得間隔は0.2秒以上にしてください")
        if not math.isfinite(wait_seconds) or wait_seconds <= 0:
            raise WorkflowError("待機上限は0より大きくしてください")
        self.workflow = workflow
        self.emit = emit
        self.interval = interval
        self.wait_seconds = wait_seconds
        self.phase = "setting"
        self.day = 0 if start_day is None else start_day
        self.resume_day = start_day is not None
        self.checkpoint = checkpoint
        self.retryable = True
        self.setting = workflow.work_dir / "setting.json"
        self.agents = workflow.work_dir / "agents.json"
        self.progress = workflow.work_dir / "automatic-progress.json"
        self._started = False

    def _save(self) -> None:
        paths = [self.setting, self.agents]
        if self.phase == "send_agents":
            paths.append(self.workflow.work_dir / "selected-agents.json")
        if self.phase in ("solve", "send_answer"):
            paths.append(self.workflow.work_dir / f"state-day{self.day}.json")
        if self.phase == "send_answer":
            paths.extend(self.workflow.work_dir / name for name in
                         ("answer.json", "score-next.json", "prepared-answer.json"))
        if self.checkpoint is not None:
            paths.append(self.checkpoint)
        saved = {
            "version": 1, "identity": self.workflow.identity(),
            "phase": self.phase, "day": self.day,
            "resume_day": self.resume_day, "days": getattr(self, "days", None),
            "checkpoint": str(self.checkpoint) if self.checkpoint else None,
            "files": {str(path.resolve()): file_hash(path) for path in paths if path.is_file()},
        }
        atomic_write(self.progress, json.dumps(saved).encode())

    @classmethod
    def restore(cls, workflow: Workflow, emit: Callable[[str, Any], None],
                interval: float = 0.5, wait_seconds: float = 3600) -> AutomaticMatch:
        runner = cls(workflow, emit, interval=interval, wait_seconds=wait_seconds)
        saved = read_json(runner.progress)
        if not isinstance(saved, dict) or saved.get("version") != 1 or saved.get("identity") != workflow.identity():
            raise WorkflowError("復旧記録の接続先・TOKENが一致しません")
        if (not isinstance(saved.get("files"), dict) or type(saved.get("day")) is not int
                or saved["day"] < 0 or "checkpoint" not in saved or "resume_day" not in saved
                or "days" not in saved):
            raise WorkflowError("復旧記録が不正です")
        for name, digest in saved["files"].items():
            path = Path(name)
            if not path.is_file() or file_hash(path) != digest:
                raise WorkflowError(f"復旧用ファイルが変更・削除されています: {path.name}")
        if saved["phase"] not in ("setting", "select", "send_agents", "state", "solve", "send_answer", "done"):
            raise WorkflowError("復旧記録の段階が不正です")
        if saved["phase"] != "setting" and (
                type(saved["days"]) is not int or saved["days"] <= 0
                or saved["day"] > saved["days"]):
            raise WorkflowError("復旧記録の日数が不正です")
        runner.phase, runner.day = saved["phase"], saved["day"]
        runner.days, runner.resume_day = saved["days"], saved["resume_day"]
        runner.checkpoint = Path(saved["checkpoint"]) if saved["checkpoint"] else None
        runner.state = workflow.work_dir / f"state-day{runner.day}.json"
        runner._started = True
        return runner

    def _wait_fetch(self, kind: str, path: Path) -> None:
        w = self.workflow
        if kind == "state":
            w.wait_for_state(
                self.day, path,
                interval=self.interval,
                wait_seconds=self.wait_seconds,
            )
            return
        if kind == "setting":
            w.wait_for_setting(
                path, interval=self.interval, wait_seconds=self.wait_seconds,
            )
            return
        raise WorkflowError(f"未対応の取得種別です: {kind}")

    def run(self) -> None:
        if not self._started and self.progress.exists():
            raise WorkflowError("保存済み自動実行があります。停止位置から再開するか、新しい作業フォルダを使用してください")
        self._started = True
        try:
            self._run_loop()
        except WorkflowError as error:
            if str(error).startswith("connection failed:"):
                guidance = (
                    "通信timeout(s)を15へ増やし、作業フォルダを消さずに"
                    "「試合終了まで全自動」をもう一度押してください。"
                )
                if not self.retryable:
                    guidance += (
                        " 送信結果が不明なため自動再送はしていません。再開時は"
                        "保存済みの同一回答だけを検査して送信します。"
                    )
                raise WorkflowError(f"{error}\n{guidance}") from error
            raise
        finally:
            self._save()

    def _run_loop(self) -> None:
        w = self.workflow
        while self.phase != "done":
            self._save()
            w.check_stop()
            phase = self.phase
            labels = {"setting": "問題情報の公開待ち・取得", "select": "エージェント種別選択",
                      "send_agents": "種別送信", "state": "当日マップの公開待ち・取得",
                      "solve": "探索", "send_answer": "回答送信"}
            self.emit("phase", f"自動: {labels[phase]} / day={self.day + 1}")
            if phase == "setting":
                self._wait_fetch("setting", self.setting)
                days = read_json(self.setting).get("daySteps", [])
                if not days or not 0 <= self.day < len(days):
                    raise WorkflowError("試合日数または開始dayが不正です")
                self.days = len(days)
                self.phase = "state" if self.resume_day else "select"
                self.emit("setting", self.setting)
            elif phase == "select":
                self.emit("log", w.select_agents(self.setting, self.agents).output)
                self.phase = "send_agents"
                self.emit("agents", self.agents)
            elif phase == "send_agents":
                w.check_stop()
                self.retryable = False
                w.send_agents(self.agents, self.setting)
                self.phase = "state"
                self.retryable = True
                self.emit("log", "エージェント種別を送信しました")
            elif phase == "state":
                self.state = w.work_dir / f"state-day{self.day}.json"
                self._wait_fetch("state", self.state)
                if self.day > 0 and w.shared_dir is not None:
                    self.checkpoint = w.work_dir / f"score-day{self.day}.json"
                    source, revision = w.import_checkpoint(
                        self.day, self.setting, self.checkpoint
                    )
                    self.emit(
                        "log",
                        f"共有チェックポイントを取得: revision={revision} {source}",
                    )
                self.phase = "solve"
                self.emit("state", self.state)
            elif phase == "solve":
                self.emit("solve_start", self.day)
                with w.artifact_lock:
                    result = w.solve_day(self.setting, self.state, self.checkpoint)
                self.emit("log", result.output)
                self.emit("solved", result)
                self.phase = "send_answer"
                self.emit("answer", w.work_dir / "answer.json")
                # 回答経路は当日開始位置から描いた後、状態表だけでなく
                # マップ上の車両・スポット在庫も探索後状態へ更新する。
                self.emit("result_state", w.work_dir / "result-state.json")
            elif phase == "send_answer":
                w.check_stop()
                self.retryable = False
                with w.artifact_lock:
                    revision, _, self.checkpoint = w.send_answer(
                        self.day, w.work_dir / "answer.json", w.work_dir / "score-next.json",
                        self.state, self.setting,
                    )
                self.day += 1
                self.phase = "done" if self.day == self.days else "state"
                self.retryable = True
                self.emit("accepted", (revision, self.checkpoint, self.day))
        self.emit("phase", "全日程の送信が完了しました")
