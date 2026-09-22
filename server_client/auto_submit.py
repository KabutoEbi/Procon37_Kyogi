#!/usr/bin/env python3
"""設定取得、種別送信、全日探索・送信を自動実行する。"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from get_problem import ApiError, load_configured_dotenv, wait_and_fetch
from send_answer import send


def run_solver(solver: Path, arguments: list[str], work_dir: Path) -> None:
    command = [str(solver), *arguments]
    completed = subprocess.run(command, cwd=work_dir, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"solver exited with status {completed.returncode}: {command}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--env-file", type=Path, default=Path(".env"))
    result.add_argument("--url", default=os.environ.get("PROCON_URL"))
    result.add_argument("--token", default=os.environ.get("PROCON_TOKEN"))
    result.add_argument("--solver", type=Path, default=Path("./main"))
    result.add_argument("--work-dir", type=Path, default=Path("runtime"))
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
    if args.interval < 0.2:
        print("--interval must be at least 0.2 seconds", file=sys.stderr)
        return 2

    solver = args.solver.expanduser().resolve()
    if not solver.is_file():
        print(f"solver not found: {solver}", file=sys.stderr)
        return 2
    work_dir = args.work_dir.expanduser().resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    log_dir = work_dir / "logs"
    setting_path = work_dir / "setting.json"
    agents_path = work_dir / "agents.json"

    try:
        setting = wait_and_fetch(
            args.url,
            args.token,
            "setting",
            setting_path,
            wait_seconds=args.wait,
            interval=args.interval,
            timeout=args.timeout,
            log_dir=log_dir,
        )
        run_solver(
            solver,
            ["select", str(setting_path), str(agents_path)],
            work_dir,
        )
        send(
            args.url,
            args.token,
            "agent",
            agents_path,
            timeout=args.timeout,
            log_dir=log_dir,
        )
        print("agent kinds accepted", flush=True)

        day_count = len(setting.get("daySteps", []))
        if day_count <= 0:
            raise RuntimeError("setting has no days")
        checkpoint: Path | None = None
        for day in range(day_count):
            state_path = work_dir / f"state-day{day}.json"
            wait_and_fetch(
                args.url,
                args.token,
                "state",
                state_path,
                wait_seconds=args.wait,
                interval=args.interval,
                timeout=args.timeout,
                log_dir=log_dir,
                expected_day=day,
            )
            command = ["solve", str(setting_path), str(state_path)]
            if checkpoint is not None:
                command.append(str(checkpoint))
            run_solver(solver, command, work_dir)

            answer = work_dir / "answer.json"
            candidate_score = work_dir / "score-next.json"
            archived_answer = work_dir / f"answer-day{day}.json"
            shutil.copy2(answer, archived_answer)
            revision = send(
                args.url,
                args.token,
                "action",
                archived_answer,
                timeout=args.timeout,
                log_dir=log_dir,
            )
            checkpoint = work_dir / f"score-day{day + 1}.json"
            shutil.copy2(candidate_score, checkpoint)
            print(
                f"day={day} accepted revision={revision} "
                f"checkpoint={checkpoint.name}",
                flush=True,
            )
            if day + 1 < day_count:
                time.sleep(args.interval)
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
