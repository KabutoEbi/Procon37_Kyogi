#!/usr/bin/env python3
"""同梱簡易サーバーと実C++ソルバーを使う結合テスト。"""

from __future__ import annotations

import os
import socket
import sys
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "visualizer"))

from backend import (  # noqa: E402
    ConnectionConfig,
    DEFAULT_SIMPLE_CONFIG,
    SearchParameters,
    SimpleServer,
    Workflow,
    WorkflowError,
)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


@unittest.skipUnless(
    os.environ.get("PROCON_TEST_SIMPLE_SERVER") and os.environ.get("PROCON_TEST_SOLVER"),
    "PROCON_TEST_SIMPLE_SERVER=1とPROCON_TEST_SOLVERを指定すると実サーバー連携を検査",
)
class SimpleServerIntegrationTest(unittest.TestCase):
    def test_day_zero_submission(self) -> None:
        solver = Path(os.environ["PROCON_TEST_SOLVER"]).resolve()
        self.assertTrue(solver.is_file(), solver)
        self.assertTrue(DEFAULT_SIMPLE_CONFIG.is_file(), DEFAULT_SIMPLE_CONFIG)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            server = SimpleServer(root / "server")
            try:
                info = server.start(DEFAULT_SIMPLE_CONFIG, 0, free_port(), 8, 0.5)
                parameters = SearchParameters(
                    beam_width=16,
                    joint_beam_width=32,
                    threads=1,
                    time_limit_ms=600,
                    safety_margin_ms=100,
                    max_actions=3,
                    trial_candidates=0,
                    trial_beam_width=8,
                    trial_joint_beam_width=16,
                    selection_time_ms=600,
                    send_reserve_ms=1000,
                    clock_margin_ms=100,
                )
                work = root / "client"
                workflow = Workflow(
                    ConnectionConfig(info.url, info.token, 2), solver, work, parameters
                )
                setting = work / "setting.json"
                agents = work / "agents.json"
                state = work / "state-day0.json"

                self.assertIn("成功", workflow.check_connection())
                workflow.fetch_setting(setting)
                workflow.select_agents(setting, agents)
                workflow.send_agents(agents, setting)

                deadline = time.monotonic() + 15
                while True:
                    try:
                        workflow.fetch_state(0, state)
                        break
                    except WorkflowError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.2)

                workflow.solve_day(setting, state)
                revision, answer, checkpoint = workflow.send_answer(
                    0,
                    work / "answer.json",
                    work / "score-next.json",
                    state,
                    setting,
                )
                self.assertEqual(revision, 1)
                self.assertTrue(answer.is_file())
                self.assertTrue(checkpoint.is_file())
            finally:
                server.stop()
            self.assertFalse(server.running)


if __name__ == "__main__":
    unittest.main()
