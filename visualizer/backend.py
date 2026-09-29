"""GUIから利用する回答システム・ソルバー操作の薄いバックエンド。"""

from __future__ import annotations

import json
import hashlib
import math
import os
import platform
import shutil
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Callable


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SERVER_CLIENT_DIR = PROJECT_ROOT / "server_client"
if str(SERVER_CLIENT_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_CLIENT_DIR))

from get_problem import ApiError, fetch, request_json, atomic_write  # noqa: E402
from send_answer import send  # noqa: E402


class WorkflowError(RuntimeError):
    """GUIに表示できる操作エラー。"""


class Paused(WorkflowError):
    pass


@dataclass(frozen=True)
class SearchParameters:
    beam_width: int = 128
    joint_beam_width: int = 512
    threads: int = 0
    time_limit_ms: int = 5000
    safety_margin_ms: int = 1000
    max_actions: int = 4
    macro_actions: int = 1
    adaptive_beam: int = 1
    trial_candidates: int = 12
    trial_beam_width: int = 32
    trial_joint_beam_width: int = 128
    selection_time_ms: int = 60000
    send_reserve_ms: int = 12000
    clock_margin_ms: int = 1000

    def arguments(self) -> list[str]:
        result = []
        for name, value in vars(self).items():
            if type(value) is not int or not 0 <= value <= 2147483647:
                raise WorkflowError(f"{name}: 0以上の整数を指定してください")
            if name.endswith("width") and value == 0:
                raise WorkflowError(f"{name}: 1以上を指定してください")
            if name in ("macro_actions", "adaptive_beam") and value not in (0, 1):
                raise WorkflowError(f"{name}: 0または1を指定してください")
            if name not in ("send_reserve_ms", "clock_margin_ms"):
                result.extend(["--" + name.replace("_", "-"), str(value)])
        return result


@dataclass(frozen=True)
class RandomSettingParameters:
    width: int = 16
    height: int = 16
    agents: int = 4
    spots: int = 16
    brands: int = 12
    days: int = 4
    seconds: int = 60
    teams: int = 2
    seed: int = 1

    def arguments(self) -> list[str]:
        values = vars(self)
        limits = {
            "width": (8, 32), "height": (8, 32),
            "agents": (3, 8), "days": (4, 10),
            "seconds": (1, 2147483647), "teams": (1, 2147483647),
            "seed": (0, 18446744073709551615),
        }
        for name, (lower, upper) in limits.items():
            value = values[name]
            if type(value) is not int or not lower <= value <= upper:
                raise WorkflowError(f"{name}: {lower}〜{upper}で指定してください")
        maximum_spots = max(self.width, self.height)
        if type(self.spots) is not int or not self.agents <= self.spots <= maximum_spots:
            raise WorkflowError(
                f"spots: agents以上、マップの長辺{maximum_spots}以下で指定してください"
            )
        if type(self.brands) is not int or not 1 <= self.brands <= self.spots:
            raise WorkflowError("brands: 1以上spots以下で指定してください")
        result: list[str] = []
        for name, value in values.items():
            result.extend(["--" + name, str(value)])
        return result


@dataclass(frozen=True)
class ConnectionConfig:
    url: str
    token: str
    timeout: float = 10.0

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
        "daily-brands", "total-udon", "match-brands",
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


def connection_from_env(path: Path, timeout: float = 10.0) -> ConnectionConfig:
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
    for spot in setting.get("spots", []):
        if isinstance(spot, dict):
            spots[int(spot["pos"])] = (int(spot["brand"]), int(spot["stocks"]))

    traffics: dict[int, int] = {}
    agents: tuple[AgentView, ...] = tuple(
        AgentView(index, -1, int(pos), int(setting.get("fuelLimits", 0)))
        for index, pos in enumerate(setting.get("agents", []))
    )
    day: int | None = None
    if state_path is not None and state_path.is_file():
        state = read_json(state_path)
        if not isinstance(state, dict):
            raise WorkflowError("当日状態はJSONオブジェクトではありません")
        if type(state.get("day")) is int:
            day = state["day"]
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
    )


def map_acquisition_totals(setting_path: Path) -> tuple[int, int]:
    """設定時点のうどん総数と、重複を除いた系列総数を返す。"""
    snapshot = load_snapshot(setting_path)
    total_udon = sum(stocks for _, stocks in snapshot.spots.values())
    total_brands = len({brand for brand, _ in snapshot.spots.values()})
    return total_udon, total_brands


class Workflow:
    def __init__(
        self,
        connection: ConnectionConfig,
        solver: Path,
        work_dir: Path,
        parameters: SearchParameters | None = None,
        stop: threading.Event | None = None,
    ) -> None:
        self.connection = connection
        self.solver = solver.expanduser().resolve()
        self.work_dir = work_dir.expanduser().resolve()
        self.work_dir.mkdir(parents=True, exist_ok=True)
        self.log_dir = self.work_dir / "logs"
        self.parameters = parameters or SearchParameters()
        self.stop = stop or threading.Event()

    def check_stop(self) -> None:
        if self.stop.is_set():
            raise Paused("停止しました。自動実行は同じ段階から再開できます")

    def _run(self, arguments: list[str], *, cwd: Path | None = None,
             ends_at: float = 0, day_seconds: float = 0,
             reserve_for_submission: bool = False) -> CommandResult:
        self.check_stop()
        self.parameters.arguments()  # GUI以外から指定した値も検査する。
        reserve = max(self.parameters.send_reserve_ms,
                      math.ceil(self.connection.timeout * 1000) +
                      self.parameters.clock_margin_ms + 1200)
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
        extra = margin - self.parameters.safety_margin_ms
        parameters = replace(
            self.parameters, safety_margin_ms=margin,
            time_limit_ms=min(2147483647, self.parameters.time_limit_ms + extra)
                if self.parameters.time_limit_ms else 0,
            selection_time_ms=min(2147483647, self.parameters.selection_time_ms + extra)
                if self.parameters.selection_time_ms else 0,
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
            remaining = ends_at - time.time() - reserve / 1000
            if remaining <= 0:
                raise WorkflowError("送信時間を確保できないため探索を開始しません")
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
            "-O2",
            "-pthread",
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
        if not self.solver.is_file():
            raise WorkflowError(f"ソルバーがありません: {self.solver}")
        output = output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + ".generate")
        temporary.unlink(missing_ok=True)
        command = [
            str(self.solver), "generate", str(temporary),
            *parameters.arguments(),
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
            temporary.replace(output)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
        return result

    def fetch_setting(self, output: Path) -> Path:
        self.connection.validate()
        try:
            fetch(
                self.connection.url,
                self.connection.token,
                "setting",
                output,
                timeout=self.connection.timeout,
                log_dir=self.log_dir,
            )
        except (ApiError, OSError, ValueError) as error:
            raise WorkflowError(str(error)) from error
        return output

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
        if value.get("day") != day:
            downloaded.unlink(missing_ok=True)
            raise WorkflowError(
                f"取得した状態はday={value.get('day')}です（期待値: {day}）"
            )
        downloaded.replace(output)
        return output

    def select_agents(self, setting: Path, output: Path) -> CommandResult:
        self._require_no_pending("agents")
        marker = self.work_dir / "selected-agents.json"
        marker.unlink(missing_ok=True)
        if not self.solver.is_file():
            raise WorkflowError(f"ソルバーがありません: {self.solver}")
        result = self._run(["select", str(setting.resolve()), str(output.resolve())],
                           ends_at=read_json(setting).get("startsAt", 0))
        atomic_write(marker, json.dumps({
            "setting": file_hash(setting), "agents": file_hash(output), "identity": self.identity(),
        }).encode())
        return result

    def solve_day(
        self,
        setting: Path,
        state: Path,
        checkpoint: Path | None = None,
    ) -> CommandResult:
        self._require_no_pending(f"day{read_json(state)['day']}")
        self._invalidate_answer()
        if not self.solver.is_file():
            raise WorkflowError(f"ソルバーがありません: {self.solver}")
        daily = read_json(state)
        day = daily["day"]
        setting_value = read_json(setting)
        durations = setting_value.get("daySeconds")
        if not isinstance(durations, list) or not 0 <= day < len(durations):
            raise WorkflowError("設定から当日の試合時間を取得できません")
        day_duration = durations[day]
        if type(day_duration) not in (int, float):
            raise WorkflowError("設定のdaySecondsが不正です")
        if day > 0 and checkpoint is None:
            raise WorkflowError("day 1以降は得点チェックポイントが必要です")
        if checkpoint is not None and read_json(checkpoint).get("nextDay") != day:
            raise WorkflowError("チェックポイントの日番号が当日状態と一致しません")
        arguments = ["solve", str(setting.resolve()), str(state.resolve())]
        if checkpoint is not None:
            arguments.append(str(checkpoint.resolve()))
        result = self._run(
            arguments,
            ends_at=daily.get("endsAt", 0),
            day_seconds=float(day_duration),
            reserve_for_submission=True,
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
        if path.exists():
            saved = read_json(path)
            if saved["receipt"] != receipt:
                raise WorkflowError("別の送信記録があります。復旧では同じ候補だけを使用してください。別試合は新しい作業フォルダを使用してください")
            else:
                return path, saved
        payload_path = self.work_dir / f"submitted-{name}.json"
        score_path = self.work_dir / f"submitted-{name}-score.json"
        atomic_write(payload_path, payload.read_bytes())
        if score is not None:
            atomic_write(score_path, score.read_bytes())
        saved = {"receipt": receipt, "status": "pending", "revision": None}
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
        if not self.solver.is_file():
            raise WorkflowError(f"ソルバーがありません: {self.solver}")
        setting_value = read_json(setting)
        durations = setting_value.get("daySeconds")
        if not isinstance(durations, list) or not durations or type(durations[0]) not in (int, float):
            raise WorkflowError("設定から1日の試合時間を取得できません")
        result = self._run(
            [str(setting.resolve())],
            day_seconds=float(durations[0]),
        )
        if not result_state.is_file():
            raise WorkflowError("ソルバーが探索後状態 result-state.json を生成しませんでした")
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
        deadline = self._submission_deadline(
            read_json(setting).get("startsAt", 0) if setting is not None else 0)
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
        saved["status"] = "accepted"
        atomic_write(journal, json.dumps(saved).encode())

    def send_answer(
        self,
        day: int,
        answer: Path,
        candidate_score: Path,
        state: Path | None = None,
        setting: Path | None = None,
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
        journal, saved = self._submission(f"day{day}", receipt, answer, candidate_score)
        frozen_answer = self.work_dir / f"submitted-day{day}.json"
        frozen_score = self.work_dir / f"submitted-day{day}-score.json"
        if file_hash(frozen_answer) != receipt["answer"] or file_hash(frozen_score) != receipt["score"]:
            raise WorkflowError("保存済み送信候補が変更されています")
        atomic_write(archived_answer, frozen_answer.read_bytes())
        checkpoint = self.work_dir / f"score-day{day + 1}.json"
        if saved["status"] == "accepted":
            atomic_write(checkpoint, frozen_score.read_bytes())
            return saved["revision"], archived_answer, checkpoint
        deadline = self._submission_deadline(receipt.get("endsAt", 0))
        # 再送は明示的な再開時だけ。同じ日でなければ結果不明のまま停止。
        if saved.get("attempted"):
            current = self.work_dir / "recovery-state.json"
            self.fetch_state(day, current)
            if state is None or read_json(current) != read_json(state):
                raise WorkflowError("当日状態が変わっています。結果不明の回答は再送しません")
        saved["attempted"] = True
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
        saved.update(status="accepted", revision=revision)
        atomic_write(journal, json.dumps(saved).encode())
        atomic_write(checkpoint, frozen_score.read_bytes())
        return revision, archived_answer, checkpoint


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class AutomaticMatch:
    """停止時は段階を保持する。POSTに失敗した場合は自動再送しない。"""

    def __init__(self, workflow: Workflow, emit: Callable[[str, Any], None],
                 start_day: int | None = None, checkpoint: Path | None = None,
                 interval: float = 0.5, wait_seconds: float = 3600) -> None:
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
        deadline = time.monotonic() + self.wait_seconds
        while True:
            w.check_stop()
            try:
                if kind == "setting":
                    w.fetch_setting(path)
                    return
                # fetch_stateの期待日検査前に古い日か先の日かを区別する。
                downloaded = path.with_name(path.name + ".poll")
                w.connection.validate()
                daily = fetch(w.connection.url, w.connection.token, "state", downloaded,
                              timeout=w.connection.timeout, log_dir=w.log_dir)
                actual = daily.get("day")
                if type(actual) is not int:
                    raise WorkflowError("当日状態のdayが不正です")
                if actual > self.day:
                    raise WorkflowError(f"サーバーはday={actual}です。期待したday={self.day}を過ぎました")
                if actual == self.day:
                    downloaded.replace(path)
                    return
                downloaded.unlink(missing_ok=True)
            except (WorkflowError, ApiError) as error:
                cause = error if isinstance(error, ApiError) else error.__cause__
                if not isinstance(cause, ApiError) or cause.status not in (0, 403):
                    raise
            if time.monotonic() >= deadline:
                raise WorkflowError(f"{kind}の公開待ちがタイムアウトしました")
            w.stop.wait(self.interval)

    def run(self) -> None:
        if not self._started and self.progress.exists():
            raise WorkflowError("保存済み自動実行があります。停止位置から再開するか、新しい作業フォルダを使用してください")
        self._started = True
        try:
            self._run_loop()
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
            self.emit("phase", f"自動: {labels[phase]} / day={self.day}")
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
                self.phase = "solve"
                self.emit("state", self.state)
            elif phase == "solve":
                self.emit("solve_start", self.day)
                self.emit("log", w.solve_day(self.setting, self.state, self.checkpoint).output)
                self.phase = "send_answer"
                self.emit("answer", w.work_dir / "answer.json")
            elif phase == "send_answer":
                w.check_stop()
                self.retryable = False
                revision, _, self.checkpoint = w.send_answer(
                    self.day, w.work_dir / "answer.json", w.work_dir / "score-next.json",
                    self.state, self.setting,
                )
                self.day += 1
                self.phase = "done" if self.day == self.days else "state"
                self.retryable = True
                self.emit("accepted", (revision, self.checkpoint, self.day))
        self.emit("phase", "全日程の送信が完了しました")
