"""公開API詳細キャッシュから16×16の上位目標を集計する。"""

import argparse
import json
import statistics
from pathlib import Path


def score(result):
    return result["kinds"], result["dks"], result["balls"]


def summarize(matches):
    rounds = {}
    accepted = {}
    for match in matches:
        if not isinstance(match, dict) or match.get("status") != "scored" or not match.get("verified"):
            continue
        p = match.get("problem", {})
        field = p.get("map", {})
        if (field.get("width"), field.get("height"), len(p.get("agents", []))) != (16, 16, 4):
            continue
        if not match.get("results"):
            continue
        accepted[match["id"]] = match
    for match in accepted.values():
        key = match.get("round_id", match["id"])
        top = max(match["results"], key=score)
        if key not in rounds or score(top) > score(rounds[key]["top"]):
            rounds[key] = {"match": match, "top": top}
    rows = []
    for key, entry in sorted(rounds.items()):
        match, top = entry["match"], entry["top"]
        p = match["problem"]
        brands = len({s["brand"] for s in p["spots"]})
        capacity = [sum(min(sum(min(s["stocks"], patrols) for s in p["spots"]),
                            patrols * steps) for steps in p["daySteps"])
                    for patrols in range(1, 5)]
        target = max(top["balls"], top["dks"], top["kinds"])
        lower = next((i + 1 for i, cap in enumerate(capacity) if cap >= target), None)
        rows.append({"round": key, "match": match["id"], "target": list(score(top)),
                     "leader": top.get("label"), "days": len(p["daySteps"]),
                     "steps": p["daySteps"], "fuel": p["fuelLimits"],
                     "stock_per_day": sum(s["stocks"] for s in p["spots"]),
                     "full_brands": score(top)[:2] == (brands, brands * len(p["daySteps"])),
                     "patrol_capacity": capacity, "minimum_patrols": lower})
    return {"matches": len(accepted), "unique_rounds": len(rows),
            "full_brand_rounds": sum(r["full_brands"] for r in rows),
            "median_leader_balls": statistics.median(r["target"][2] for r in rows) if rows else None,
            "rounds": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()
    print(json.dumps(summarize(json.loads(Path(p).read_text()) for p in args.paths),
                     ensure_ascii=False, indent=2))
