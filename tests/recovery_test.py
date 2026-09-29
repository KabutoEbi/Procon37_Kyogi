"""永続復旧・締切管理の回帰テスト（外部通信なし）。"""

import json
import sys
import tempfile
import time
import unittest
import subprocess
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "visualizer"))
from backend import (AutomaticMatch, Workflow, WorkflowError, ConnectionConfig,
                     SearchParameters, ApiError, file_hash, read_json)
import get_problem


class RecoveryTest(unittest.TestCase):
    def prepare(self, work):
        w = Workflow(ConnectionConfig("http://example", "secret", 2), ROOT / "main", work)
        setting, state = work / "setting.json", work / "state-day0.json"
        setting.write_text('{"daySteps":[1]}')
        state.write_text(json.dumps({"day": 0, "endsAt": time.time() + 60}))
        answer, score = work / "answer.json", work / "score-next.json"
        answer.write_text("[[-1]]")
        score.write_text('{"nextDay":1}')
        receipt = {"day": 0, "endsAt": read_json(state)["endsAt"],
                   "identity": w.identity(), "setting": file_hash(setting),
                   "state": file_hash(state), "answer": file_hash(answer), "score": file_hash(score)}
        (work / "prepared-answer.json").write_text(json.dumps(receipt))
        runner = AutomaticMatch(w, lambda *_: None)
        runner.phase, runner.days, runner.state = "send_answer", 1, state
        return w, runner, answer, score, state, setting

    def test_unknown_post_restart_resends_identical_and_commits(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, runner, answer, score, state, _ = self.prepare(work)
            with mock.patch("backend.send", side_effect=ApiError(0, "response lost")):
                with self.assertRaises(WorkflowError):
                    runner.run()
            self.assertFalse((work / "score-day1.json").exists())
            self.assertEqual(read_json(work / "submission-day0.json")["status"], "pending")
            restored = AutomaticMatch.restore(w, lambda *_: None)
            self.assertEqual(restored.phase, "send_answer")

            def fetch(day, path):
                self.assertEqual(day, 0)
                path.write_bytes(state.read_bytes())

            with mock.patch.object(w, "fetch_state", side_effect=fetch), \
                 mock.patch("backend.send", return_value=3) as post:
                restored.run()
                self.assertEqual(post.call_count, 1)
                self.assertEqual(post.call_args.args[3].read_bytes(), answer.read_bytes())
            self.assertEqual(read_json(work / "submission-day0.json")["revision"], 3)
            self.assertEqual((work / "score-day1.json").read_bytes(), score.read_bytes())
            self.assertEqual(AutomaticMatch.restore(w, lambda *_: None).phase, "done")

    def test_accepted_receipt_repairs_crash_before_checkpoint_without_post(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, runner, answer, score, _, _ = self.prepare(work)
            original = __import__("backend").atomic_write

            def write(path, data):
                if path.name == "score-day1.json":
                    raise OSError("disk failure")
                original(path, data)

            with mock.patch("backend.send", return_value=2), mock.patch("backend.atomic_write", side_effect=write):
                with self.assertRaises(OSError):
                    runner.run()
            restored = AutomaticMatch.restore(w, lambda *_: None)
            with mock.patch("backend.send") as post:
                restored.run()
                post.assert_not_called()
            self.assertEqual((work / "score-day1.json").read_bytes(), score.read_bytes())

    def test_unknown_result_blocks_new_search_and_future_day(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, runner, answer, score, state, setting = self.prepare(work)
            with mock.patch("backend.send", side_effect=ApiError(0, "lost")):
                with self.assertRaises(WorkflowError):
                    runner.run()
            with self.assertRaisesRegex(WorkflowError, "保護"):
                w.solve_day(setting, state)
            with mock.patch.object(w, "fetch_state", side_effect=WorkflowError("day advanced")), \
                 mock.patch("backend.send") as post:
                with self.assertRaises(WorkflowError):
                    AutomaticMatch.restore(w, lambda *_: None).run()
                post.assert_not_called()
            self.assertFalse((work / "score-day1.json").exists())

    def test_restore_rejects_changed_files_and_connection(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, runner, answer, *_ = self.prepare(work)
            runner._save()
            other = Workflow(ConnectionConfig("http://other", "secret"), ROOT / "main", work)
            with self.assertRaises(WorkflowError):
                AutomaticMatch.restore(other, lambda *_: None)
            answer.write_text("[[-2]]")
            with self.assertRaises(WorkflowError):
                AutomaticMatch.restore(w, lambda *_: None)

    def test_solver_reserves_timeout_without_erasing_relative_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            w = Workflow(ConnectionConfig("http://example", "secret", 10), Path(sys.executable),
                         Path(directory), SearchParameters(time_limit_ms=5000, send_reserve_ms=0))
            process = mock.Mock(returncode=0)
            process.communicate.return_value = ("", "")
            with mock.patch("backend.subprocess.Popen", return_value=process) as spawn:
                result = w._run([], ends_at=time.time() + 60)
            command = result.command
            margin = int(command[command.index("--safety-margin-ms") + 1])
            limit = int(command[command.index("--time-limit-ms") + 1])
            self.assertEqual(margin, 12200)
            self.assertEqual(limit - margin, 4000)
            with mock.patch("backend.subprocess.Popen") as spawn:
                with self.assertRaises(WorkflowError):
                    w._run([], ends_at=time.time() + 1)
                spawn.assert_not_called()

    def test_solver_time_is_capped_below_day_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            w = Workflow(
                ConnectionConfig("http://example", "secret", 1),
                Path(sys.executable),
                Path(directory),
                SearchParameters(time_limit_ms=60000, safety_margin_ms=0),
            )
            process = mock.Mock(returncode=0)
            process.communicate.return_value = ("", "")
            with mock.patch("backend.subprocess.Popen", return_value=process):
                result = w._run([], day_seconds=30)
            command = result.command
            margin = int(command[command.index("--safety-margin-ms") + 1])
            limit = int(command[command.index("--time-limit-ms") + 1])
            self.assertEqual(margin, 1000)
            self.assertEqual(limit, 30000)
            self.assertLess(limit - margin, 30000)

    def test_rate_limit_wait_rechecks_deadline_before_post(self):
        with mock.patch("get_problem.time.monotonic", return_value=0), \
             mock.patch("get_problem.time.sleep"), \
             mock.patch("get_problem.time.time", return_value=11), \
             mock.patch("get_problem.urllib.request.urlopen") as http:
            with self.assertRaises(ApiError):
                get_problem.request_json("http://example", "secret", "/", method="POST", deadline=10)
            http.assert_not_called()

    def test_watchdog_terminates_overdue_solver(self):
        with tempfile.TemporaryDirectory() as directory:
            w = Workflow(ConnectionConfig("http://example", "secret", 1), Path(sys.executable),
                         Path(directory), SearchParameters(send_reserve_ms=0, clock_margin_ms=0))
            process = mock.Mock()
            process.communicate.side_effect = [subprocess.TimeoutExpired("solver", .1), ("", "")]
            with mock.patch("backend.subprocess.Popen", return_value=process), \
                 mock.patch("backend.time.monotonic", side_effect=[0, 100]):
                with self.assertRaisesRegex(WorkflowError, "未完成回答"):
                    w._run([], ends_at=time.time() + 10)
            process.terminate.assert_called_once()

    def test_expired_pending_never_posts_or_confirms(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, runner, *_ = self.prepare(work)
            with mock.patch("backend.send", side_effect=ApiError(0, "lost")):
                with self.assertRaises(WorkflowError):
                    runner.run()
            restored = AutomaticMatch.restore(w, lambda *_: None)
            with mock.patch("backend.time.time", return_value=time.time() + 120), \
                 mock.patch("backend.send") as post:
                with self.assertRaises(WorkflowError):
                    restored.run()
                post.assert_not_called()
            self.assertFalse((work / "score-day1.json").exists())

    def test_kind_submission_receipt_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w = Workflow(ConnectionConfig("http://example", "secret"), ROOT / "main", work)
            setting, agents = work / "setting.json", work / "agents.json"
            setting.write_text(json.dumps({"startsAt": time.time() + 60}))
            agents.write_text("[0,1]")
            (work / "selected-agents.json").write_text(json.dumps({
                "identity": w.identity(), "setting": file_hash(setting), "agents": file_hash(agents)}))
            with mock.patch("backend.send", side_effect=ApiError(0, "lost")):
                with self.assertRaises(WorkflowError):
                    w.send_agents(agents, setting)
            self.assertEqual(read_json(work / "submission-agents.json")["status"], "pending")
            with mock.patch("backend.send", return_value=None) as post:
                w.send_agents(agents, setting)
                post.assert_called_once()
            with mock.patch("backend.send") as post:
                w.send_agents(agents, setting)
                post.assert_not_called()


if __name__ == "__main__":
    unittest.main()
