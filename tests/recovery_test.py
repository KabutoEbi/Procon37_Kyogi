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
from backend import (AutomaticMatch, Workflow, WorkflowError, Paused, ConnectionConfig,
                     SearchParameters, ApiError, agent_selection_seconds,
                     file_hash, read_json)
import get_problem
import send_answer


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

    def test_agent_selection_deadline_uses_first_fetch_and_board_tier(self):
        self.assertEqual(agent_selection_seconds({"map": {"width": 16}}), 60)
        self.assertEqual(agent_selection_seconds({"map": {"width": 24}}), 90)
        self.assertEqual(agent_selection_seconds({"map": {"width": 32}}), 120)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = work / "setting.json"
            w = Workflow(ConnectionConfig("http://example", "secret"), ROOT / "main", work)
            value = {"map": {"width": 24}, "startsAt": 0}

            def fetch_setting(*args, **kwargs):
                args[3].write_text(json.dumps(value))
                return value

            with mock.patch("backend.fetch", side_effect=fetch_setting), \
                 mock.patch("backend.time.time", side_effect=(100.0, 200.0)):
                w.fetch_setting(output)
                first = read_json(work / "setting-received.json")
                w.fetch_setting(output)
                second = read_json(work / "setting-received.json")
            self.assertEqual(first["receivedAt"], 100.0)
            self.assertEqual(first["selectionEndsAt"], 190.0)
            self.assertEqual(second, first)

    def test_setting_button_waits_until_problem_is_published(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = work / "setting.json"
            w = Workflow(ConnectionConfig("http://example", "secret"), ROOT / "main", work)
            attempts = 0

            def fetch_after_publication(*args, **kwargs):
                nonlocal attempts
                attempts += 1
                if attempts < 3:
                    raise ApiError(403, "not published")
                value = {"map": {"width": 16}, "startsAt": 0}
                args[3].write_text(json.dumps(value))
                return value

            with mock.patch("backend.fetch", side_effect=fetch_after_publication), \
                 mock.patch.object(w.stop, "wait") as wait:
                self.assertEqual(w.wait_for_setting(output), output)
            self.assertEqual(attempts, 3)
            self.assertEqual(wait.call_count, 2)

            w.stop.set()
            with mock.patch("backend.fetch") as fetch:
                with self.assertRaises(Paused):
                    w.wait_for_setting(output)
                fetch.assert_not_called()

    def test_accepted_checkpoint_is_shared_and_latest_revision_is_imported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            work, shared = root / "work", root / "shared"
            work.mkdir()
            shared.mkdir()
            _, _, answer, score, state, setting = self.prepare(work)
            w = Workflow(
                ConnectionConfig("http://example", "secret", 2),
                ROOT / "main", work, shared_dir=shared,
            )
            with mock.patch("backend.send", return_value=7):
                _, _, checkpoint = w.send_answer(
                    0, answer, score, state, setting
                )
            packages = list(shared.glob("checkpoint-*-day1-revision7.json"))
            self.assertEqual(len(packages), 1)

            checkpoint.write_text(json.dumps({"nextDay": 1, "totalUdon": 9}))
            w.publish_checkpoint(checkpoint, setting, 8)
            imported = root / "imported-score.json"
            source, revision = w.import_checkpoint(1, setting, imported)
            self.assertEqual(revision, 8)
            self.assertIn("revision8", source.name)
            self.assertEqual(read_json(imported)["totalUdon"], 9)

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

    def test_accepted_answer_can_be_replaced_on_same_day(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, _, answer, score, state, setting = self.prepare(work)
            with mock.patch("backend.send", return_value=1):
                w.send_answer(0, answer, score, state, setting)
            old_payload = (work / "submitted-day0.json").read_bytes()
            old_score = (work / "score-day1.json").read_bytes()
            answer.write_text("[[-2]]")
            score.write_text('{"nextDay":1,"totalUdon":2}')
            receipt_path = work / "prepared-answer.json"
            receipt = read_json(receipt_path)
            receipt.update(answer=file_hash(answer), score=file_hash(score))
            receipt_path.write_text(json.dumps(receipt))
            with mock.patch("backend.send", side_effect=ApiError(0, "lost")):
                with self.assertRaises(WorkflowError):
                    w.send_answer(0, answer, score, state, setting)
            self.assertEqual((work / "submitted-day0.json").read_bytes(), old_payload)
            self.assertEqual((work / "score-day1.json").read_bytes(), old_score)
            self.assertEqual(read_json(work / "submission-day0-accepted-1.json")["revision"], 1)
            with self.assertRaisesRegex(WorkflowError, "保護"):
                w.solve_day(setting, state)
            next_state = work / "state-day1.json"
            next_state.write_text('{"day":1}')
            with self.assertRaisesRegex(WorkflowError, "保護"):
                w.solve_day(setting, next_state, work / "score-day1.json")
            def fetch(day, path):
                path.write_bytes(state.read_bytes())
            with mock.patch.object(w, "fetch_state", side_effect=fetch), \
                 mock.patch("backend.send", return_value=2) as post:
                w.send_answer(0, answer, score, state, setting)
                post.assert_called_once()
            self.assertEqual(read_json(work / "submission-day0.json")["revision"], 2)
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
                         Path(directory), SearchParameters(time_limit_ms=10000, send_reserve_ms=0))
            process = mock.Mock(returncode=0)
            process.communicate.return_value = ("", "")
            with mock.patch("backend.subprocess.Popen", return_value=process) as spawn:
                result = w._run([], ends_at=time.time() + 60)
            command = result.command
            margin = int(command[command.index("--safety-margin-ms") + 1])
            limit = int(command[command.index("--time-limit-ms") + 1])
            self.assertEqual(margin, 12200)
            self.assertEqual(limit - margin, 5000)
            with mock.patch("backend.subprocess.Popen") as spawn:
                with self.assertRaises(WorkflowError):
                    w._run([], ends_at=time.time() + 1)
                spawn.assert_not_called()

    def test_agent_selection_uses_recorded_window_until_submission_reserve(self):
        with tempfile.TemporaryDirectory() as directory:
            w = Workflow(
                ConnectionConfig("http://example", "secret", 10),
                Path(sys.executable), Path(directory),
                SearchParameters(
                    selection_time_ms=0, safety_margin_ms=5000,
                    send_reserve_ms=0,
                ),
            )
            process = mock.Mock(returncode=0)
            process.communicate.return_value = ("", "")
            with mock.patch("backend.time.time", return_value=40.0), \
                 mock.patch("backend.subprocess.Popen", return_value=process):
                result = w._run(
                    [], ends_at=100.0, reserve_for_submission=True,
                )
            command = result.command
            margin = int(command[command.index("--safety-margin-ms") + 1])
            limit = int(command[command.index("--selection-time-ms") + 1])
            self.assertEqual(margin, 12200)
            self.assertEqual(limit, 60000)
            self.assertEqual(limit - margin, 47800)

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

    def test_manual_follower_keeps_five_second_reserve(self):
        with tempfile.TemporaryDirectory() as directory:
            w = Workflow(
                ConnectionConfig("http://example", "secret", 10),
                Path(sys.executable),
                Path(directory),
                SearchParameters(time_limit_ms=60000, safety_margin_ms=5000),
            )
            process = mock.Mock(returncode=0)
            process.communicate.return_value = ("", "")
            with mock.patch("backend.subprocess.Popen", return_value=process):
                result = w._run(
                    [], ends_at=time.time() + 60, day_seconds=60,
                    reserve_network_timeout=False,
                )
            command = result.command
            margin = int(command[command.index("--safety-margin-ms") + 1])
            limit = int(command[command.index("--time-limit-ms") + 1])
            self.assertEqual(margin, 5000)
            self.assertEqual(limit - margin, 55000)

    def test_equal_score_replacement_is_not_sent(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, _, answer, score, state, setting = self.prepare(work)
            score.write_text(json.dumps({
                "nextDay": 1, "matchBrands": 3,
                "sumOfDailyBrandCounts": 3, "totalUdon": 10,
            }))
            receipt_path = work / "prepared-answer.json"
            receipt = read_json(receipt_path)
            receipt["score"] = file_hash(score)
            receipt_path.write_text(json.dumps(receipt))
            with mock.patch("backend.send", return_value=1):
                w.send_answer(0, answer, score, state, setting)

            answer.write_text("[[-2]]")
            receipt["answer"] = file_hash(answer)
            receipt_path.write_text(json.dumps(receipt))
            with mock.patch("backend.send") as post:
                with self.assertRaisesRegex(WorkflowError, "タイブレーク"):
                    w.send_answer(0, answer, score, state, setting)
                post.assert_not_called()
            self.assertEqual(read_json(work / "submission-day0.json")["revision"], 1)

    def test_wait_and_fetch_does_not_publish_wrong_day(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "state.json"
            output.write_text('{"day":-1}')
            days = iter((0, 1))

            def fetch_old_then_expected(*args, **kwargs):
                value = {"day": next(days)}
                args[3].write_text(json.dumps(value))
                return value

            with mock.patch("get_problem.fetch", side_effect=fetch_old_then_expected), \
                 mock.patch("get_problem.time.sleep"):
                value = get_problem.wait_and_fetch(
                    "http://example", "secret", "state", output,
                    expected_day=1, wait_seconds=1, interval=1, timeout=1,
                )
            self.assertEqual(value["day"], 1)
            self.assertEqual(read_json(output)["day"], 1)

            def fetch_future(*args, **kwargs):
                value = {"day": 2}
                args[3].write_text(json.dumps(value))
                return value

            with mock.patch("get_problem.fetch", side_effect=fetch_future):
                with self.assertRaisesRegex(ValueError, "passed expected"):
                    get_problem.wait_and_fetch(
                        "http://example", "secret", "state", output,
                        expected_day=0, wait_seconds=1, interval=1, timeout=1,
                    )
            self.assertEqual(read_json(output)["day"], 1)

    def test_post_audit_logs_request_and_error_response(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = root / "answer.json"
            log_dir = root / "logs"
            payload.write_text("[[-1]]")
            with mock.patch(
                "send_answer.request_json",
                side_effect=ApiError(400, "bad", '{"code":"bad"}'),
            ):
                with self.assertRaises(ApiError):
                    send_answer.send(
                        "http://example", "secret", "action", payload,
                        log_dir=log_dir,
                    )
            names = [path.name for path in log_dir.iterdir()]
            self.assertTrue(any("post-action-request" in name for name in names))
            self.assertTrue(any("post-action-error-response" in name for name in names))

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

    def test_tls_timeout_reports_safe_resume_guidance(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w, runner, *_ = self.prepare(work)
            with mock.patch(
                "backend.send",
                side_effect=ApiError(
                    0, "connection failed: _ssl.c:1064: "
                       "The handshake operation timed out"),
            ):
                with self.assertRaisesRegex(
                    WorkflowError, "timeout\\(s\\).*15.*同一回答"
                ):
                    runner.run()
            self.assertEqual(runner.phase, "send_answer")
            self.assertFalse(runner.retryable)
            self.assertTrue((work / "automatic-progress.json").is_file())
            self.assertEqual(
                read_json(work / "submission-day0.json")["status"], "pending")

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
