"""公開API詳細のキャッシュを集計する。問題データ自体はリポジトリに保存しない。

python3 tests/analyze_arena_history.py /tmp/procon37-*.json
同じround_idのテーブルをまとめ、1928以降はモデル検証用に取り置く。
"""

import argparse
import json
import math
import statistics
from pathlib import Path


def capacity(problem, patrols):
    stocks = sum(min(s["stocks"], patrols) for s in problem["spots"])
    return sum(min(stocks, patrols * steps) for steps in problem["daySteps"])


def features(problem):
    size = max(problem["map"]["width"], problem["map"]["height"])
    steps = statistics.mean(problem["daySteps"])
    duplicates = 1 - len({s["brand"] for s in problem["spots"]}) / len(problem["spots"])
    return [1, steps / size, problem["fuelLimits"] / steps,
            duplicates, int(size == 24), int(size == 32)]


def fit(rows):
    # 小さい正規方程式を標準ライブラリだけで解く。
    n = len(rows[0]["features"])
    matrix = [[sum(r["features"][i] * r["features"][j] for r in rows)
               for j in range(n)] +
              [sum(r["features"][i] * r["capture"] for r in rows)]
              for i in range(n)]
    for i in range(n):
        pivot = max(range(i, n), key=lambda j: abs(matrix[j][i]))
        matrix[i], matrix[pivot] = matrix[pivot], matrix[i]
        divisor = matrix[i][i]
        if abs(divisor) < 1e-10:
            raise ValueError("特徴量の独立性が不足しています")
        matrix[i] = [v / divisor for v in matrix[i]]
        for j in range(n):
            if i == j:
                continue
            factor = matrix[j][i]
            matrix[j] = [a - factor * b for a, b in zip(matrix[j], matrix[i])]
    return [matrix[i][-1] for i in range(n)]


def predict(coefficients, row):
    return max(0, min(1, sum(a * b for a, b in zip(coefficients, row["features"]))))


def official_score(result):
    return (result["kinds"], result["dks"], result["balls"])


def round_leaders(matches):
    rounds = {}
    for match in matches:
        if not match.get("results"):
            continue
        top = max(match["results"], key=official_score)
        key = match.get("round_id", match["id"])
        if key not in rounds or official_score(top) > official_score(rounds[key][1]):
            rounds[key] = (match, top)
    return rounds


def analyze(paths):
    matches = {}
    for path in paths:
        value = json.loads(Path(path).read_text())
        if not isinstance(value, dict) or "problem" not in value:
            continue
        if not 1833 <= value.get("id", 0) <= 1931:
            continue
        if value.get("status") != "scored" or not value.get("verified"):
            continue
        matches[value["id"]] = value
    rounds = round_leaders(matches.values())
    rows = []
    for key, (match, top) in sorted(rounds.items()):
        p = match["problem"]
        stock = len(p["daySteps"]) * sum(s["stocks"] for s in p["spots"])
        brands = len({s["brand"] for s in p["spots"]})
        # 玉数だけでなく、日別系列の必要取得数も下限に含める。
        required = max(top["balls"], top["dks"], top["kinds"])
        lower = next(k for k in range(1, len(p["agents"]) + 1)
                     if capacity(p, k) >= required)
        rows.append({"round": key, "id": match["id"], "features": features(p),
                     "capture": top["balls"] / stock, "stock": stock,
                     "score": [top["kinds"], top["dks"], top["balls"]],
                     "minimum_patrols": lower,
                     "full_daily_brands": top["dks"] == brands * len(p["daySteps"])})
    train = [r for r in rows if r["round"] < 1928]
    holdout = [r for r in rows if r["round"] >= 1928]
    coefficients = fit(train)
    loo = [abs(predict(fit(train[:i] + train[i + 1:]), r) - r["capture"])
           for i, r in enumerate(train)]
    for row in holdout:
        row["predicted_balls"] = math.ceil(predict(coefficients, row) * row["stock"])
    return {"matches": len(matches), "unique_rounds": len(rows),
            "training_rounds": len(train), "coefficients": coefficients,
            "leave_one_round_out_capture_mae": statistics.mean(loo),
            "full_daily_brands": sum(r["full_daily_brands"] for r in rows),
            "holdout": holdout}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()
    print(json.dumps(analyze(args.paths), ensure_ascii=False, indent=2))
