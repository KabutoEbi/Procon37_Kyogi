#!/usr/bin/env python3
"""全日程の自動送信。--resumeで保存済み停止位置から復旧する。"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "visualizer"))
from backend import (AutomaticMatch, ConnectionConfig, SearchParameters,
                     Workflow, WorkflowError, read_json)
from get_problem import load_configured_dotenv


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
    result.add_argument("--resume", action="store_true",
                        help="送信結果不明の場合は同じ日・同じ回答だけを再送")
    result.add_argument("--start-day", type=int)
    result.add_argument("--checkpoint", type=Path)
    result.add_argument("--time-limit-ms", type=int, default=50000)
    result.add_argument("--auto-beam-width", type=int, choices=(0, 1), default=1)
    result.add_argument("--send-reserve-ms", type=int, default=12000)
    result.add_argument("--clock-margin-ms", type=int, default=1000)
    return result


def main() -> int:
    load_configured_dotenv()
    args = parser().parse_args()
    try:
        if args.resume and (args.start_day is not None or args.checkpoint is not None):
            raise WorkflowError("--resume cannot be combined with --start-day/--checkpoint")
        connection = ConnectionConfig(args.url or "", args.token or "", args.timeout)
        connection.validate()
        parameters = SearchParameters(time_limit_ms=args.time_limit_ms,
                                      auto_beam_width=args.auto_beam_width,
                                      send_reserve_ms=args.send_reserve_ms,
                                      clock_margin_ms=args.clock_margin_ms)
        parameters.arguments()
        workflow = Workflow(connection, args.solver, args.work_dir, parameters)
        if not workflow.solver.is_file():
            raise WorkflowError(f"solver not found: {workflow.solver}")
        if args.start_day is not None and args.start_day > 0:
            if args.checkpoint is None or read_json(args.checkpoint).get("nextDay") != args.start_day:
                raise WorkflowError("開始dayに対応する--checkpointが必要です")

        def emit(kind, value):
            if kind in ("phase", "log"):
                print(str(value).replace(connection.token, "[TOKEN]"), flush=True)
            elif kind == "accepted":
                revision, checkpoint, day = value
                print(f"day={day - 1} accepted revision={revision} checkpoint={checkpoint.name}", flush=True)

        if args.resume:
            runner = AutomaticMatch.restore(workflow, emit, args.interval, args.wait)
        else:
            runner = AutomaticMatch(workflow, emit, args.start_day,
                                    args.checkpoint.resolve() if args.checkpoint else None,
                                    args.interval, args.wait)
        runner.run()
    except (WorkflowError, OSError, ValueError) as error:
        message = str(error)
        if args.token:
            message = message.replace(args.token, "[TOKEN]")
        print(f"error: {message}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
