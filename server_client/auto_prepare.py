#!/usr/bin/env python3
"""サーバーから取得してmainを実行するが、回答は送信しない。"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from get_problem import ApiError, load_configured_dotenv, wait_and_fetch


def default_solver() -> Path:
    return Path("./main.exe" if os.name == "nt" else "./main")


def run_solver(solver: Path, arguments: list[str], work_dir: Path) -> None:
    completed = subprocess.run(
        [str(solver), *arguments], cwd=work_dir, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"solver exited with status {completed.returncode}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("mode", choices=("select", "day"))
    result.add_argument("--env-file", type=Path, default=Path(".env"))
    result.add_argument("--url", default=os.environ.get("PROCON_URL"))
    result.add_argument("--token", default=os.environ.get("PROCON_TOKEN"))
    result.add_argument("--solver", type=Path, default=default_solver())
    result.add_argument("--work-dir", type=Path, default=Path("runtime"))
    result.add_argument("--score", type=Path)
    result.add_argument("--expected-day", type=int)
    result.add_argument("--wait", type=float, default=3600.0)
    result.add_argument("--interval", type=float, default=0.5)
    result.add_argument("--timeout", type=float, default=10.0)
    return result


def main() -> int:
    load_configured_dotenv()
    args = parser().parse_args()
    if not args.url or not args.token:
        print("--url/PROCON_URL and --token/PROCON_TOKEN are required", file=sys.stderr)
        return 2
    solver = args.solver.expanduser().resolve()
    if not solver.is_file():
        print(f"solver not found: {solver}", file=sys.stderr)
        return 2
    work_dir = args.work_dir.expanduser().resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    log_dir = work_dir / "logs"
    setting_path = work_dir / "setting.json"

    try:
        wait_and_fetch(
            args.url,
            args.token,
            "setting",
            setting_path,
            wait_seconds=args.wait,
            interval=args.interval,
            timeout=args.timeout,
            log_dir=log_dir,
        )
        if args.mode == "select":
            run_solver(
                solver,
                ["select", str(setting_path), str(work_dir / "agents.json")],
                work_dir,
            )
            print("prepared agents.json; nothing was sent")
            return 0

        state_path = work_dir / "state.json"
        state = wait_and_fetch(
            args.url,
            args.token,
            "state",
            state_path,
            wait_seconds=args.wait,
            interval=args.interval,
            timeout=args.timeout,
            log_dir=log_dir,
            expected_day=args.expected_day,
        )
        day = state.get("day")
        if type(day) is not int:
            raise RuntimeError("state has no integer day")
        command = ["solve", str(setting_path), str(state_path)]
        if day > 0:
            if args.score is None:
                raise RuntimeError("--score is required after day 0")
            command.append(str(args.score.expanduser().resolve()))
        run_solver(solver, command, work_dir)
        print(
            f"prepared day={day} answer.json and score-next.json; "
            "nothing was sent"
        )
    except (
        ApiError,
        OSError,
        RuntimeError,
        TimeoutError,
        ValueError,
        json.JSONDecodeError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
