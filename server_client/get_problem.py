#!/usr/bin/env python3
"""GET /setting または GET / のJSONを保存する。"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


_last_request_at = 0.0
_minimum_request_interval = 0.2


class ApiError(RuntimeError):
    def __init__(self, status: int, message: str, body: str = "") -> None:
        super().__init__(message)
        self.status = status
        self.body = body


def load_dotenv(path: Path) -> None:
    """単純なKEY=VALUE形式を読む。既存の環境変数は上書きしない。"""
    if not path.is_file():
        return
    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise ValueError(f"{path}:{line_number}: expected KEY=VALUE")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or not key.replace("_", "A").isalnum() or key[0].isdigit():
            raise ValueError(f"{path}:{line_number}: invalid variable name")
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        os.environ.setdefault(key, value)


def load_configured_dotenv(arguments: list[str] | None = None) -> Path:
    values = sys.argv[1:] if arguments is None else arguments
    path = Path(os.environ.get("PROCON_ENV_FILE", ".env"))
    for index, value in enumerate(values):
        if value == "--env-file" and index + 1 < len(values):
            path = Path(values[index + 1])
        elif value.startswith("--env-file="):
            path = Path(value.split("=", 1)[1])
    load_dotenv(path.expanduser())
    return path


def request_json(
    base_url: str,
    token: str,
    endpoint: str,
    *,
    method: str = "GET",
    payload: Any = None,
    timeout: float = 10.0,
    deadline: float | None = None,
) -> tuple[Any, bytes, int]:
    global _last_request_at
    remaining = _minimum_request_interval - (time.monotonic() - _last_request_at)
    if remaining > 0:
        time.sleep(remaining)
    if deadline is not None:
        timeout = min(timeout, deadline - time.time())
        if timeout <= 0:
            raise ApiError(0, "request deadline has passed (not sent)")
    url = base_url.rstrip("/") + endpoint
    data = None
    headers = {
        "Accept": "application/json",
        "Procon-Token": token,
        "User-Agent": "procon37-python-client/1.0",
    }
    if payload is not None:
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(
        url, data=data, headers=headers, method=method)
    _last_request_at = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            status = response.status
    except urllib.error.HTTPError as error:
        raw = error.read()
        body = raw.decode("utf-8", errors="replace")
        raise ApiError(error.code, f"HTTP {error.code}: {body}", body) from error
    except urllib.error.URLError as error:
        raise ApiError(0, f"connection failed: {error.reason}") from error
    except (TimeoutError, OSError) as error:
        raise ApiError(0, f"connection failed: {error}") from error

    if not raw.strip():
        return None, raw, status
    try:
        return json.loads(raw), raw, status
    except json.JSONDecodeError as error:
        body = raw.decode("utf-8", errors="replace")
        raise ApiError(status, f"invalid JSON response: {error}", body) from error


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def save_raw_log(directory: Path | None, name: str, raw: bytes) -> None:
    if directory is None:
        return
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    suffix = f"{time.time_ns() % 1_000_000_000:09d}"
    atomic_write(directory / f"{timestamp}-{suffix}-{name}.json", raw)


def fetch(
    base_url: str,
    token: str,
    kind: str,
    output: Path,
    *,
    timeout: float = 10.0,
    log_dir: Path | None = None,
) -> Any:
    endpoint = "/setting" if kind == "setting" else "/"
    value, raw, _ = request_json(
        base_url, token, endpoint, timeout=timeout)
    if not isinstance(value, dict):
        raise ApiError(200, f"{endpoint} response must be a JSON object")
    atomic_write(output, raw + (b"" if raw.endswith(b"\n") else b"\n"))
    save_raw_log(log_dir, f"get-{kind}", raw)
    return value


def wait_and_fetch(
    base_url: str,
    token: str,
    kind: str,
    output: Path,
    *,
    wait_seconds: float,
    interval: float,
    timeout: float,
    log_dir: Path | None = None,
    expected_day: int | None = None,
) -> Any:
    if interval < 0.2:
        raise ValueError("interval must be at least 0.2 seconds")
    deadline = time.monotonic() + wait_seconds
    while True:
        try:
            value = fetch(
                base_url, token, kind, output,
                timeout=timeout, log_dir=log_dir)
            if expected_day is None or value.get("day") == expected_day:
                return value
        except ApiError as error:
            if error.status not in (0, 403):
                raise
        if wait_seconds <= 0 or time.monotonic() >= deadline:
            raise TimeoutError(f"timed out waiting for {kind}")
        time.sleep(interval)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("kind", choices=("setting", "state"))
    result.add_argument("output", type=Path)
    result.add_argument("--env-file", type=Path, default=Path(".env"))
    result.add_argument("--url", default=os.environ.get("PROCON_URL"))
    result.add_argument("--token", default=os.environ.get("PROCON_TOKEN"))
    result.add_argument("--wait", type=float, default=0.0)
    result.add_argument("--interval", type=float, default=0.5)
    result.add_argument("--timeout", type=float, default=10.0)
    result.add_argument("--expected-day", type=int)
    result.add_argument("--log-dir", type=Path)
    return result


def main() -> int:
    load_configured_dotenv()
    args = parser().parse_args()
    if not args.url or not args.token:
        print("--url/PROCON_URL and --token/PROCON_TOKEN are required", file=sys.stderr)
        return 2
    try:
        value = wait_and_fetch(
            args.url,
            args.token,
            args.kind,
            args.output,
            wait_seconds=args.wait,
            interval=args.interval,
            timeout=args.timeout,
            log_dir=args.log_dir,
            expected_day=args.expected_day,
        )
    except (ApiError, OSError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    detail = f" day={value.get('day')}" if args.kind == "state" else ""
    print(f"saved {args.kind} to {args.output}{detail}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
