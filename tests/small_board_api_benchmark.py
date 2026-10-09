"""16×16のAPI問題で指定した1機能を同じ条件・交互順で比較する。"""

import argparse
import json
import subprocess
from pathlib import Path

from analyze_small_board_history import score


def run_case(binary, match, args, enabled):
    problem = match["problem"]
    command = [str(binary), "-", str(args.time_ms), str(args.supply_mask),
               args.traffic, str(args.beam_width), str(args.beam_width * 4),
               "1", "0", "2", "1", "0", "1", "0", "25", "1", "1"]
    if args.feature == "final-columns":
        command.extend((str(enabled), "1"))
    else:
        command.extend(("1", str(enabled)))
    result = subprocess.run(command, input=json.dumps(problem), text=True,
                            capture_output=True, timeout=args.timeout, check=True)
    lines = result.stdout.splitlines()
    final = next(line for line in lines if line.startswith("result="))
    value = [int(part) for part in final[7:].split("/")]
    days = [line.split(",") for line in lines if line and line[0].isdigit()]
    return {"score": value, "day_ms": [int(day[5]) for day in days]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--binary", type=Path, default=Path(".build/tools/arena_benchmark"))
    parser.add_argument("--time-ms", type=int, default=0)
    parser.add_argument("--beam-width", type=int, default=32)
    parser.add_argument("--supply-mask", type=int, default=1)
    parser.add_argument("--traffic", choices=("smooth", "mixed", "mixed-after-first"),
                        default="mixed-after-first")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--feature", choices=("final-columns", "quantity-first"),
                        default="final-columns")
    args = parser.parse_args()
    if args.time_ms < 0 or args.beam_width < 1 or not 0 <= args.supply_mask < 15 or args.timeout < 1:
        parser.error("invalid benchmark budget, width or supply mask")
    rows = []
    totals = [0, 0, 0]
    for index, path in enumerate(args.paths):
        match = json.loads(Path(path).read_text())
        p = match["problem"]
        if (p["map"]["width"], p["map"]["height"], len(p["agents"])) != (16, 16, 4):
            parser.error(f"not a first-round setting: {path}")
        values = {}
        for future in ((0, 1) if index % 2 == 0 else (1, 0)):
            values[future] = run_case(args.binary, match, args, future)
        old, new = values[0]["score"], values[1]["score"]
        totals[0 if new > old else 1 if new == old else 2] += 1
        row = {"match": match["id"], "round": match.get("round_id", match["id"]),
               "target": list(score(max(match["results"], key=score))),
               "before": values[0], "after": values[1]}
        rows.append(row)
        print(json.dumps(row, ensure_ascii=False), flush=True)
    print(json.dumps({"wins_ties_losses": totals, "cases": len(rows),
                      "traffic": args.traffic, "time_ms": args.time_ms,
                      "supply_mask": args.supply_mask,
                      "feature": f"small-board-{args.feature}"}, ensure_ascii=False))
