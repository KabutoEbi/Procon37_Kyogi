#!/usr/bin/env python3
"""GUIバックエンドのヘッドレス回帰テスト。"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import threading
import subprocess
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "visualizer"))

from backend import (  # noqa: E402
    ConnectionConfig,
    Workflow,
    WorkflowError,
    AgentView,
    MapSnapshot,
    AutomaticMatch,
    SearchParameters,
    RandomSettingParameters,
    SimpleServer,
    Paused,
    ApiError,
    file_hash,
    format_json_for_display,
    read_json,
    connection_from_env,
    day_time_limit_ms,
    daily_acquisition_theoretical,
    load_snapshot,
    match_acquisition_theoretical,
    map_acquisition_totals,
    parse_solver_result,
    trace_answer_routes,
    simple_server_teams,
)


class BackendTest(unittest.TestCase):
    def test_app_icon_assets(self) -> None:
        svg = ROOT / "visualizer/assets/app_icon.svg"
        png = ROOT / "visualizer/assets/app_icon.png"
        self.assertTrue(svg.is_file())
        self.assertTrue(png.is_file())
        self.assertEqual(png.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")

    def test_format_json_for_display_groups_simple_arrays(self) -> None:
        value = {
            "day": 0,
            "routes": [[2, -31, 2], [-32]],
            "agents": [
                {"kind": 0, "pos": 3, "fuel": 40},
                {"kind": 1, "pos": 7, "fuel": 20},
            ],
        }
        self.assertEqual(
            format_json_for_display(value),
            """{
  \"day\": 0,
  \"routes\": [
    [2, -31, 2],
    [-32]
  ],
  \"agents\": [
    {\"kind\": 0, \"pos\": 3, \"fuel\": 40},
    {\"kind\": 1, \"pos\": 7, \"fuel\": 20}
  ]
}""",
        )
        self.assertEqual(format_json_for_display([1, 2, 3]), "[1, 2, 3]")
        self.assertEqual(json.loads(format_json_for_display(value)), value)

    def test_random_setting_parameters(self) -> None:
        defaults = RandomSettingParameters()
        self.assertEqual(
            (defaults.width, defaults.height, defaults.agents,
             defaults.spots, defaults.brands, defaults.days),
            (32, 32, 8, 32, 20, 10),
        )
        parameters = RandomSettingParameters(
            width=8, height=12, agents=3, spots=8, brands=5,
            days=4, seconds=30, teams=2, seed=99,
        )
        self.assertEqual(parameters.arguments()[-2:], ["--seed", "99"])
        with self.assertRaisesRegex(WorkflowError, "spots"):
            RandomSettingParameters(agents=5, spots=4).arguments()
        with self.assertRaisesRegex(WorkflowError, "brands"):
            RandomSettingParameters(spots=8, brands=9).arguments()
        with self.assertRaisesRegex(WorkflowError, "include_congestion"):
            RandomSettingParameters(include_congestion=2).arguments()
        self.assertNotIn(
            "--include_congestion",
            RandomSettingParameters(include_congestion=1).arguments(),
        )

    def test_day_time_limit_ms(self) -> None:
        self.assertEqual(SearchParameters().time_limit_ms, 60000)
        self.assertIn("--auto-beam-width", SearchParameters(auto_beam_width=1).arguments())
        with self.assertRaisesRegex(WorkflowError, "auto_beam_width"):
            SearchParameters(auto_beam_width=2).arguments()
        setting = {"daySeconds": [60, 12.5, 3_000_000]}
        self.assertEqual(day_time_limit_ms(setting, 0), 60000)
        self.assertEqual(day_time_limit_ms(setting, 1), 12500)
        self.assertEqual(day_time_limit_ms(setting, 2), 2147483647)
        with self.assertRaisesRegex(WorkflowError, "当日の試合時間"):
            day_time_limit_ms(setting, 3)

    def test_map_acquisition_totals(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            setting = Path(directory) / "setting.json"
            setting.write_text(json.dumps({
                "map": {"width": 2, "height": 1, "cells": [[0, 0]]},
                "spots": [
                    {"pos": 0, "brand": 7, "stocks": 3},
                    {"pos": 1, "brand": 7, "stocks": 2},
                ],
            }), encoding="utf-8")
            self.assertEqual(map_acquisition_totals(setting), (5, 1))

    def test_daily_acquisition_theoretical(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            setting = Path(directory) / "setting.json"
            setting.write_text(json.dumps({
                "map": {"width": 3, "height": 1, "cells": [[0, 0, 0]]},
                "spots": [
                    {"pos": 0, "brand": 7, "stocks": 3},
                    {"pos": 1, "brand": 7, "stocks": 1},
                    {"pos": 2, "brand": 9, "stocks": 2},
                ],
                "daySteps": [1, 4],
            }), encoding="utf-8")
            self.assertEqual(daily_acquisition_theoretical(setting, 0, 2), (2, 2))
            self.assertEqual(daily_acquisition_theoretical(setting, 1, 2), (5, 2))
            self.assertEqual(daily_acquisition_theoretical(setting, 1, 0), (0, 0))
            with self.assertRaisesRegex(WorkflowError, "ステップ数"):
                daily_acquisition_theoretical(setting, 2, 2)

    def test_match_acquisition_theoretical_sums_days(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            setting = Path(directory) / "setting.json"
            setting.write_text(json.dumps({
                "map": {"width": 3, "height": 1, "cells": [[0, 0, 0]]},
                "spots": [
                    {"pos": 0, "brand": 7, "stocks": 3},
                    {"pos": 1, "brand": 7, "stocks": 1},
                    {"pos": 2, "brand": 9, "stocks": 2},
                ],
                "daySteps": [1, 4],
            }), encoding="utf-8")
            self.assertEqual(
                match_acquisition_theoretical(setting, 2), (7, 4)
            )

    def test_solver_result_report(self) -> None:
        report = """parameters beam-width=19
=== procon37 solver report ===
day=2

search
  success=true
  stop=time-limit
  elapsed-ms=4987
  expanded=123456

result
  udon=17
  daily-brands=4
  match-brands=7
    total-daily-brands=9
  total-udon=42
"""
        summary = parse_solver_result(report)
        self.assertIsNotNone(summary)
        self.assertEqual(summary.day, 2)
        self.assertEqual(summary.stop, "time-limit")
        self.assertEqual(summary.elapsed_ms, 4987)
        self.assertEqual(summary.expanded, 123456)
        self.assertEqual((summary.daily_udon, summary.daily_brands), (17, 4))
        self.assertEqual((summary.total_udon, summary.match_brands), (42, 7))
        self.assertEqual(summary.total_daily_brands, 9)
        self.assertIsNone(parse_solver_result("solver output without report"))

    def test_simple_server_client_work_clear(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            server = SimpleServer(Path(directory))
            client = server.client_work_dir
            (client / "nested").mkdir(parents=True)
            (client / "setting.json").write_text("{}", encoding="utf-8")
            (client / "nested/answer.json").write_text("[]", encoding="utf-8")
            (server.work_dir / "simple-server.log").write_text("keep", encoding="utf-8")

            self.assertEqual(server.clear_client_work(), 2)
            self.assertTrue(client.is_dir())
            self.assertEqual(list(client.iterdir()), [])
            self.assertEqual((server.work_dir / "simple-server.log").read_text(), "keep")

            process = mock.Mock()
            process.poll.return_value = None
            server.process = process
            (client / "submission-agents.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(WorkflowError, "停止してから"):
                server.clear_client_work()
            self.assertTrue((client / "submission-agents.json").exists())

    def test_simple_server_team_config(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "server.json"
            path.write_text(json.dumps({"problem": {}, "teams": [
                {"name": "red", "token": "red-token"},
                {"token": "blue-token"},
            ]}), encoding="utf-8")
            self.assertEqual(
                simple_server_teams(path),
                (("red", "red-token"), ("team-1", "blue-token")),
            )

    def test_env_loading_and_environment_override(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / ".env"
            path.write_text(
                "PROCON_URL=http://file.example\nPROCON_TOKEN='file-token'\n",
                encoding="utf-8",
            )
            with mock.patch.dict(os.environ, {"PROCON_URL": "http://env.example"}, clear=True):
                config = connection_from_env(path)
            self.assertEqual(config.url, "http://env.example")
            self.assertEqual(config.token, "file-token")

    def test_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            setting = root / "setting.json"
            state = root / "state.json"
            setting.write_text(json.dumps({
                "map": {"width": 2, "height": 1, "cells": [[0, 1]]},
                "spots": [{"pos": 1, "brand": 3, "stocks": 2}],
                "daySteps": [10],
                "fuelLimits": 8,
            }), encoding="utf-8")
            state.write_text(json.dumps({
                "day": 0,
                "step": 4,
                "traffics": [{"pos": 1, "status": 2}],
                "agents": [{"kind": 0, "pos": 0, "fuel": 7}],
                "remainingStocks": [1],
            }), encoding="utf-8")
            snapshot = load_snapshot(setting, state)
            self.assertEqual((snapshot.width, snapshot.height), (2, 1))
            self.assertEqual(snapshot.spots[1], (3, 1))
            self.assertEqual(snapshot.traffics[1], 2)
            self.assertEqual(snapshot.agents[0].fuel, 7)
            self.assertEqual(snapshot.step, 4)
            self.assertEqual(snapshot.stock_limits[1], 2)
            self.assertEqual(trace_answer_routes(snapshot, [[2, -3, 5]]), ((0, 1, 0),))
            with self.assertRaises(WorkflowError):
                trace_answer_routes(snapshot, [[5]])

            state.write_text(json.dumps({"remainingStocks": []}), encoding="utf-8")
            with self.assertRaisesRegex(WorkflowError, "スポット数"):
                load_snapshot(setting, state)

        hex_snapshot = MapSnapshot(
            width=3, height=2, cells=((0, 0, 0), (0, 0, 0)),
            spots={}, traffics={}, agents=(AgentView(0, 0, 0, 5),),
            day=0, day_steps=(3,), fuel_limit=5,
        )
        self.assertEqual(trace_answer_routes(hex_snapshot, [[4, -1, 1]]), ((0, 3, 0),))

    def test_answer_checkpoint_is_confirmed_only_after_acceptance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            answer = work / "answer.json"
            score = work / "score-next.json"
            answer.write_text("[[-1]]\n", encoding="utf-8")
            score.write_text('{"nextDay":1}\n', encoding="utf-8")
            workflow = Workflow(
                ConnectionConfig("http://example", "secret"),
                ROOT / "main",
                work,
            )
            (work / "prepared-answer.json").write_text(json.dumps({
                "day": 0, "identity": workflow.identity(),
                "answer": file_hash(answer), "score": file_hash(score),
            }), encoding="utf-8")
            with mock.patch("backend.send", return_value=4):
                revision, archived, checkpoint = workflow.send_answer(0, answer, score)
            self.assertEqual(revision, 4)
            self.assertEqual(archived.read_text(encoding="utf-8"), "[[-1]]\n")
            self.assertEqual(checkpoint.read_text(encoding="utf-8"), '{"nextDay":1}\n')

            checkpoint.unlink()
            with mock.patch("backend.send") as post:
                workflow.send_answer(0, answer, score)
                post.assert_not_called()
            self.assertTrue(checkpoint.exists())

            checkpoint.unlink()
            (work / "submission-day0.json").unlink()
            with mock.patch("backend.send", side_effect=WorkflowError("rejected")):
                with self.assertRaises(WorkflowError):
                    workflow.send_answer(0, answer, score)
            self.assertFalse(checkpoint.exists())

    def test_wrong_day_does_not_replace_state_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = work / "state-day0.json"
            output.write_text('{"day":"old"}\n', encoding="utf-8")
            workflow = Workflow(
                ConnectionConfig("http://example", "secret"),
                ROOT / "main",
                work,
            )

            def fake_fetch(_url, _token, _kind, path, **_options):
                path.write_text('{"day":1}\n', encoding="utf-8")
                return {"day": 1}

            with mock.patch("backend.fetch", side_effect=fake_fetch):
                with self.assertRaises(WorkflowError):
                    workflow.fetch_state(0, output)
            self.assertEqual(output.read_text(encoding="utf-8"), '{"day":"old"}\n')
            self.assertFalse((work / "state-day0.json.download").exists())

    def test_manual_connection_probe_does_not_save_or_send(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workflow = Workflow(ConnectionConfig("http://example", "typed-token"), ROOT / "main", Path(directory))
            with mock.patch("backend.request_json", return_value=({"map": {}}, b"{}", 200)) as request:
                self.assertIn("成功", workflow.check_connection())
                self.assertEqual(request.call_args.args[1], "typed-token")
                self.assertEqual(request.call_args.args[2], "/setting")
            self.assertEqual(list(Path(directory).iterdir()), [])
            with mock.patch("backend.request_json", side_effect=ApiError(401, "unauthorized")):
                with self.assertRaises(WorkflowError):
                    workflow.check_connection()
            with mock.patch("backend.request_json", side_effect=ApiError(403, "not ready")):
                self.assertIn("未確認", workflow.check_connection())

    def test_auto_wait_pause_resume_and_post_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            workflow = Workflow(ConnectionConfig("http://example", "test"), ROOT / "main", work)
            calls = []
            events = []
            states = iter([0, 0, 1])  # 前日が返る間は待つ。

            def fake_fetch(_url, _token, kind, path, **_kwargs):
                value = {"daySteps": [4, 4]} if kind == "setting" else {"day": next(states)}
                path.write_text(json.dumps(value), encoding="utf-8")
                calls.append(kind)
                return value

            def fake_send(day, *_args):
                calls.append(f"send{day}")
                return 2, work / "answer.json", work / f"score-day{day + 1}.json"

            def emit(kind, value):
                events.append((kind, value))
                if kind == "answer" and runner.day == 0:
                    workflow.stop.set()

            runner = AutomaticMatch(workflow, emit, interval=0.2)
            with mock.patch("backend.fetch", side_effect=fake_fetch), \
                 mock.patch.object(workflow, "select_agents", return_value=mock.Mock(output="")), \
                 mock.patch.object(workflow, "send_agents") as send_agents, \
                 mock.patch.object(workflow, "solve_day", return_value=mock.Mock(output="")), \
                 mock.patch.object(workflow, "send_answer", side_effect=fake_send):
                with self.assertRaises(Paused):
                    runner.run()
                self.assertEqual(runner.phase, "send_answer")
                self.assertNotIn("send0", calls)
                workflow.stop.clear()
                runner.run()
                self.assertEqual(runner.phase, "done")
                self.assertEqual([c for c in calls if c.startswith("send")], ["send0", "send1"])
                self.assertEqual(calls.count("state"), 3)
                self.assertEqual(sum(kind == "solved" for kind, _ in events), 2)
                send_agents.assert_called_once()

            runner.phase = "send_answer"
            workflow.stop.clear()
            with mock.patch.object(workflow, "send_answer", side_effect=WorkflowError("network")) as send:
                with self.assertRaises(WorkflowError):
                    runner.run()
                send.assert_called_once()
                self.assertFalse(runner.retryable)

    def test_wrong_score_and_changed_answer_are_rejected_before_post(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w = Workflow(ConnectionConfig("http://example", "test"), ROOT / "main", work)
            answer, score = work / "answer.json", work / "score-next.json"
            answer.write_text("[[-1]]", encoding="utf-8")
            score.write_text('{"nextDay":2}', encoding="utf-8")
            receipt = {"day": 0, "identity": w.identity(), "answer": file_hash(answer), "score": file_hash(score)}
            (work / "prepared-answer.json").write_text(json.dumps(receipt), encoding="utf-8")
            with mock.patch("backend.send") as send:
                with self.assertRaises(WorkflowError):
                    w.send_answer(0, answer, score)
                answer.write_text("[[-2]]", encoding="utf-8")
                with self.assertRaises(WorkflowError):
                    w.send_answer(0, answer, score)
                send.assert_not_called()

    def test_future_day_and_auth_failure_stop_auto(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w = Workflow(ConnectionConfig("http://example", "test"), ROOT / "main", work)
            runner = AutomaticMatch(w, lambda *_: None)
            with mock.patch("backend.fetch", return_value={"day": 2}):
                with self.assertRaisesRegex(WorkflowError, "過ぎました"):
                    runner._wait_fetch("state", work / "state.json")
            with mock.patch("backend.fetch", side_effect=ApiError(401, "unauthorized")) as fetch:
                with self.assertRaises(WorkflowError):
                    runner.run()
                fetch.assert_called_once()

    def test_stop_terminates_solver_process(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            w = Workflow(ConnectionConfig("", ""), Path(sys.executable), Path(directory))
            timer = threading.Timer(0.2, w.stop.set)
            timer.start()
            processes = []
            original = subprocess.Popen

            def spawn(*args, **kwargs):
                process = original(*args, **kwargs)
                processes.append(process)
                return process

            try:
                with mock.patch("backend.subprocess.Popen", side_effect=spawn):
                    with self.assertRaises(Paused):
                        w._run(["-c", "import time; time.sleep(60)"])
                self.assertEqual(len(processes), 1)
                self.assertIsNotNone(processes[0].poll())
            finally:
                timer.cancel()

    @unittest.skipUnless(os.environ.get("PROCON_TEST_SOLVER"), "PROCON_TEST_SOLVERを指定するとC++連携も検査")
    def test_real_random_setting_generation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = work / "random-setting.json"
            workflow = Workflow(
                ConnectionConfig("", ""),
                Path(os.environ["PROCON_TEST_SOLVER"]),
                work,
            )
            workflow.generate_random_setting(
                output,
                RandomSettingParameters(
                    width=8, height=9, agents=3, spots=8, brands=5,
                    days=4, seconds=30, teams=3, seed=123,
                ),
            )
            setting = read_json(output)
            self.assertEqual((setting["map"]["width"], setting["map"]["height"]), (8, 9))
            self.assertEqual(len(setting["agents"]), 3)
            self.assertEqual(len(setting["spots"]), 8)
            self.assertEqual(len(setting["daySteps"]), 4)
            self.assertEqual(setting["players"], 3)
            workflow.parameters = SearchParameters(
                beam_width=8, joint_beam_width=32, threads=1,
                time_limit_ms=100, safety_margin_ms=0, trial_candidates=0,
            )
            for include_congestion in (0, 1):
                parameters = RandomSettingParameters(
                    width=8, height=9, agents=3, spots=8, brands=5,
                    days=4, seconds=30, teams=3, seed=123,
                    include_congestion=include_congestion,
                )
                workflow.generate_random_setting(output, parameters)
                generated = read_json(output)
                self.assertEqual(generated["map"], setting["map"])
                traffic = generated["localTraffics"]
                allowed = {0, 1, 2} if include_congestion else {0}
                self.assertEqual({t["status"] for t in traffic}, allowed)
                self.assertEqual({t["pos"] for t in traffic}, {
                    y * 8 + x for y, row in enumerate(generated["map"]["cells"])
                    for x, cell in enumerate(row) if cell == 1
                })
                workflow.generate_random_setting(output, parameters)
                self.assertEqual(read_json(output)["localTraffics"], traffic)
                result = workflow.solve_local(output)
                self.assertIn(str((work / "local-state.json").resolve()), result.command)
                self.assertEqual(read_json(work / "local-state.json")["traffics"], traffic)
                self.assertEqual(read_json(work / "result-state.json")["traffics"], traffic)
                snapshot = load_snapshot(output, work / "result-state.json")
                self.assertEqual(dict(snapshot.traffics), {t["pos"]: t["status"] for t in traffic})

    @unittest.skipUnless(os.environ.get("PROCON_TEST_SOLVER"), "PROCON_TEST_SOLVERを指定するとC++連携も検査")
    def test_real_solver_local_result_state(self) -> None:
        setting = ROOT / "sample_maps/sample_16.json"
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            workflow = Workflow(
                ConnectionConfig("http://mock", "typed-token"),
                Path(os.environ["PROCON_TEST_SOLVER"]),
                work,
                SearchParameters(
                    beam_width=16, joint_beam_width=64, threads=1,
                    time_limit_ms=100, safety_margin_ms=0, trial_candidates=0,
                ),
            )
            workflow.solve_local(setting)
            result_path = work / "result-state.json"
            result = read_json(result_path)
            source = read_json(setting)
            self.assertEqual(len(result["agents"]), len(source["agents"]))
            self.assertEqual(len(result["remainingStocks"]), len(source["spots"]))
            snapshot = load_snapshot(setting, result_path)
            self.assertEqual(
                [agent.fuel for agent in snapshot.agents],
                [agent["fuel"] for agent in result["agents"]],
            )
            self.assertEqual(
                [stock for _, stock in snapshot.spots.values()],
                result["remainingStocks"],
            )

    @unittest.skipUnless(os.environ.get("PROCON_TEST_SOLVER"), "PROCON_TEST_SOLVERを指定するとC++連携も検査")
    def test_real_solver_four_days_with_mock_http(self) -> None:
        import get_problem
        setting = json.loads((ROOT / "sample_maps/sample_16.json").read_text())
        setting["daySteps"] = setting["daySteps"][:4]
        setting["daySeconds"] = setting["daySeconds"][:4]
        current_day = 0
        kinds = None
        requests = []

        class Response:
            status = 200

            def __init__(self, value):
                self.raw = json.dumps(value).encode()

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                pass

            def read(self):
                return self.raw

        def http(request, **_kwargs):
            nonlocal current_day, kinds
            self.assertEqual(request.get_header("Procon-token"), "typed-token")
            method = request.get_method()
            requests.append((method, request.full_url))
            if request.full_url.endswith("/setting"):
                return Response(setting)
            if method == "POST":
                payload = json.loads(request.data)
                if request.full_url.endswith("/agent"):
                    kinds = payload
                    return Response(None)
                self.assertEqual(len(payload), len(kinds))
                current_day += 1
                return Response({"revision": current_day})
            agents = [dict(kind=kind, pos=pos, fuel=setting["fuelLimits"])
                      for kind, pos in zip(kinds, setting["agents"])]
            return Response({
                "day": current_day, "endsAt": 0, "agents": agents,
                "others": [{"id": i + 1, "agents": agents} for i in range(setting["players"] - 1)],
                "traffics": [{"pos": y * setting["map"]["width"] + x, "status": current_day % 3}
                             for y, row in enumerate(setting["map"]["cells"])
                             for x, cell in enumerate(row) if cell == 1],
            })

        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            w = Workflow(ConnectionConfig("http://mock", "typed-token"), Path(os.environ["PROCON_TEST_SOLVER"]), work,
                         SearchParameters(beam_width=16, joint_beam_width=64, threads=1,
                                          time_limit_ms=200, safety_margin_ms=0, trial_candidates=0))
            events = []
            runner = AutomaticMatch(w, lambda kind, value: events.append((kind, value)))
            with mock.patch.object(get_problem.urllib.request, "urlopen", side_effect=http):
                self.assertIn("成功", w.check_connection())
                runner.run()
            self.assertEqual(current_day, 4)
            self.assertEqual(read_json(work / "score-day4.json")["nextDay"], 4)
            self.assertEqual(sum(method == "POST" for method, _ in requests), 5)
            self.assertEqual(sum(kind == "state" for kind, _ in events), 4)
            self.assertEqual(sum(kind == "solved" for kind, _ in events), 4)
            logs = "\n".join(value for kind, value in events if kind == "log")
            self.assertIn(
                "beam-width=16 joint-beam-width=64 threads=1 "
                "time-limit-ms=12400 safety-margin-ms=12200",
                logs,
            )
            current_day = 2
            requests.clear()
            resumed = Workflow(w.connection, w.solver, work / "resumed", w.parameters)
            continuation = AutomaticMatch(resumed, lambda *_: None, start_day=2,
                                          checkpoint=work / "score-day2.json")
            with mock.patch.object(get_problem.urllib.request, "urlopen", side_effect=http):
                continuation.run()
            self.assertEqual(current_day, 4)
            self.assertEqual(sum(method == "POST" for method, _ in requests), 2)
            self.assertFalse(any(url.endswith("/agent") for _, url in requests))


if __name__ == "__main__":
    unittest.main()
