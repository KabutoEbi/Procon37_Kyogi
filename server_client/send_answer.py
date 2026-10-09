#!/usr/bin/env python3
"""agents.jsonまたはanswer.jsonを回答システムへ送る。"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any

from get_problem import ApiError, load_configured_dotenv, request_json, save_raw_log


def load_payload(kind: str, path: Path) -> Any:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, list) or not value:
        raise ValueError("payload must be a non-empty JSON array")
    if kind == "agent":
        if any(type(item) is not int or item not in (0, 1) for item in value):
            raise ValueError("agent payload elements must be 0 or 1")
    else:
        for plan in value:
            if not isinstance(plan, list) or not plan:
                raise ValueError("each agent plan must be a non-empty array")
            if any(type(action) is not int or action > 5 for action in plan):
                raise ValueError("actions must be integers <= 5")
    return value


def send(
    base_url: str,
    token: str,
    kind: str,
    payload_path: Path,
    *,
    timeout: float = 10.0,
    log_dir: Path | None = None,
    deadline: float | None = None,
) -> int | None:
    payload = load_payload(kind, payload_path)
    endpoint = "/agent" if kind == "agent" else "/"
    request_raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    save_raw_log(log_dir, f"post-{kind}-request", request_raw)
    try:
        response, raw, status = request_json(
            base_url,
            token,
            endpoint,
            method="POST",
            payload=payload,
            timeout=timeout,
            deadline=deadline,
        )
    except ApiError as error:
        if error.body:
            save_raw_log(
                log_dir, f"post-{kind}-error-response",
                error.body.encode("utf-8", errors="replace"),
            )
        raise
    save_raw_log(log_dir, f"post-{kind}-response", raw)
    if status != 200:
        raise ApiError(status, f"unexpected HTTP status {status}")
    if kind == "agent":
        return None
    if not isinstance(response, dict) or type(response.get("revision")) is not int:
        raise ApiError(status, "action response has no integer revision")
    revision = response["revision"]
    if revision < 0:
        raise ApiError(status, f"answer rejected: revision={revision}")
    return revision


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("kind", choices=("agent", "action"))
    result.add_argument("file", type=Path)
    result.add_argument("--env-file", type=Path, default=Path(".env"))
    result.add_argument("--url", default=os.environ.get("PROCON_URL"))
    result.add_argument("--token", default=os.environ.get("PROCON_TOKEN"))
    result.add_argument("--timeout", type=float, default=10.0)
    result.add_argument("--log-dir", type=Path)
    return result


def main() -> int:
    load_configured_dotenv()
    args = parser().parse_args()
    if not args.url or not args.token:
        print("--url/PROCON_URL and --token/PROCON_TOKEN are required", file=sys.stderr)
        return 2
    try:
        revision = send(
            args.url,
            args.token,
            args.kind,
            args.file,
            timeout=args.timeout,
            log_dir=args.log_dir,
        )
    except (ApiError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if revision is None:
        print("agent kinds accepted")
    else:
        print(f"answer accepted revision={revision}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
