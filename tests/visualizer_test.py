#!/usr/bin/env python3
"""GUIバックエンドのヘッドレス回帰テスト。"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
import threading
import subprocess
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "visualizer"))

from backend import (  # noqa: E402
    ConnectionConfig,
    DEFAULT_TIMEOUT_SECONDS,
    Workflow,
    WorkflowError,
    AgentView,
    MapSnapshot,
    AutomaticMatch,
    SearchParameters,
    RandomSettingParameters,
    SimpleServer,
    SolverResultSummary,
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
    default_solver_path,
    solver_build_command,
    process_exit_detail,
    parse_spoken_checkpoint,
    trace_answer_routes,
    simple_server_teams,
)
from app import (  # noqa: E402
    SolverGUI,
    MATCH_OPERATION_PROFILES,
    TEAM_ROLE_PROFILES,
    automatic_phase_is_waiting,
    compact_clock_status,
    duration_text,
    match_clock_values,
    match_setting_issues,
    official_score_text,
    team_role_time_limit_ms,
)
from server_client.auto_submit import parser as auto_submit_parser  # noqa: E402


class BackendTest(unittest.TestCase):
    def test_windows_solver_name_and_build_commands(self) -> None:
        with mock.patch("backend.platform.system", return_value="Windows"):
            self.assertEqual(default_solver_path(Path("work")), Path("work/main.exe"))
            with mock.patch.dict("backend.os.environ", {"CXX": "cl.exe"}, clear=False):
                command = solver_build_command(Path("main.exe"))
                self.assertEqual(command[0], "cl.exe")
                self.assertIn("/std:c++17", command)
                self.assertIn("/O2", command)
                self.assertIn("/DNDEBUG", command)
                self.assertIn("/Fe:main.exe", command)
                self.assertIn("/utf-8", command)
                self.assertTrue(any(value.startswith("/I") for value in command))
                self.assertIn("/STACK:16777216", command)
                self.assertNotIn("-pthread", command)
            with mock.patch.dict("backend.os.environ", {"CXX": "g++.exe"}, clear=False):
                command = solver_build_command(Path("main.exe"))
                self.assertEqual(command[0], "g++.exe")
                self.assertIn("-std=c++17", command)
                self.assertIn("-O3", command)
                self.assertIn("-DNDEBUG", command)
                self.assertIn("-Iinc", command)
                self.assertIn("-Wl,--stack,16777216", command)
                self.assertIn("-static-libgcc", command)
                self.assertIn("-static-libstdc++", command)
                self.assertNotIn("-pthread", command)

        with mock.patch("backend.platform.system", return_value="Darwin"), \
             mock.patch.dict("backend.os.environ", {"CXX": "c++"}, clear=False):
            self.assertEqual(default_solver_path(Path("work")), Path("work/main"))
            self.assertIn("-pthread", solver_build_command(Path("main")))

        self.assertIn("0xC00000FD", process_exit_detail(3221225725))
        self.assertIn("0xC0000374", process_exit_detail(3221226356))
        self.assertIn("0xC0000374", process_exit_detail(-1073740940))
        self.assertEqual(process_exit_detail(7), "終了コード 7")

    def test_reject_search_budget_not_exceeding_margin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            for limit in (1000, 5000):
                workflow = Workflow(ConnectionConfig("", ""), work / "main", work,
                                    SearchParameters(time_limit_ms=limit))
                with mock.patch("backend.subprocess.Popen") as process:
                    with self.assertRaisesRegex(WorkflowError, "安全余裕以下"):
                        workflow._run(["setting.json", "state.json"], day_seconds=60)
                    process.assert_not_called()
            workflow.parameters = SearchParameters(selection_time_ms=1000)
            with mock.patch("backend.subprocess.Popen") as process:
                with self.assertRaisesRegex(WorkflowError, "安全余裕以下"):
                    workflow._run(["select", "setting.json"])
                process.assert_not_called()

    def test_local_selection_ignores_past_start_without_changing_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            # 作業用コピーと同じ名前の入力でも、元ファイルを上書きしない。
            setting = work / "local-setting.json"
            source = read_json(ROOT / "sample_maps/sample_16.json")
            source["startsAt"] = 1
            setting.write_text(json.dumps(source), encoding="utf-8")
            original = setting.read_bytes()
            workflow = Workflow(ConnectionConfig("", ""), work / "main", work)

            def run(arguments, **kwargs):
                self.assertEqual(read_json(Path(arguments[1] if arguments[0] == "select"
                                               else arguments[0]))["startsAt"], 0)
                if arguments[0] == "select":
                    Path(arguments[2]).write_text(json.dumps([0] * len(source["agents"])))
                else:
                    (work / "result-state.json").write_text("{}")
                return SimpleNamespace(command=tuple(arguments))

            with mock.patch.object(workflow, "_require_solver"), \
                    mock.patch.object(workflow, "_run", side_effect=run) as executed:
                workflow.solve_local(setting)
            self.assertEqual(executed.call_count, 2)
            self.assertEqual(setting.read_bytes(), original)

    def test_default_connection_timeout_is_ten_seconds(self) -> None:
        self.assertEqual(ConnectionConfig("http://example", "secret").timeout, 10.0)
        self.assertEqual(auto_submit_parser().parse_args([]).timeout, 10.0)

    def test_three_person_presets_and_spoken_score(self) -> None:
        self.assertEqual(
            {key: (value.selection_time_ms, value.fallback_day_time_ms)
             for key, value in MATCH_OPERATION_PROFILES.items()},
            {"connection_test": (20000, 20000),
             "rehearsal": (30000, 30000),
             "production": (60000, 60000)},
        )
        self.assertEqual(
            {role: (profile.time_limit_ms, profile.production_time_limit_ms,
                    profile.udon_sa,
                    profile.stable_beam_stop, profile.sa_stagnation_stop)
             for role, profile in TEAM_ROLE_PROFILES.items()},
            {"lead": (10000, 10000, 0, 0, 0),
             "middle": (20000, 40000, 0, 1, 0),
             "long": (20000, 50000, 1, 0, 1)},
        )
        self.assertEqual(team_role_time_limit_ms("lead", "production"), 10000)
        self.assertEqual(team_role_time_limit_ms("middle", "rehearsal"), 20000)
        self.assertEqual(team_role_time_limit_ms("middle", "production"), 40000)
        self.assertEqual(team_role_time_limit_ms("long", "production"), 50000)
        summary = SolverResultSummary(
            day=2, stop="completed", elapsed_ms=1, expanded=2,
            daily_udon=7, daily_brands=5, total_udon=31,
            match_brands=11, total_daily_brands=27,
        )
        self.assertEqual(official_score_text(summary), "11 / 27 / 31")

    def test_match_setting_profile_checks(self) -> None:
        rehearsal = {
            "daySeconds": [30] * 5,
            "daySteps": [32, 36, 40, 40, 48],
            "map": {"width": 16, "height": 16},
            "agents": [1, 2, 3, 4],
            "spots": [
                {"brand": index if index < 10 else index - 10,
                 "stocks": 2 if index < 14 else 1}
                for index in range(16)
            ],
            "fuelLimits": 40,
            "busyThreshold": 1,
            "jammedThreshold": 2,
        }
        self.assertEqual(
            match_setting_issues({"daySeconds": [20] * 4}, "connection_test"),
            (),
        )
        self.assertEqual(
            match_setting_issues({"daySeconds": [20] * 5}, "connection_test"),
            ("行動回答時間または日数",),
        )
        self.assertEqual(match_setting_issues(rehearsal, "rehearsal"), ())
        first_match = json.loads(json.dumps(rehearsal))
        first_match["daySeconds"] = [60] * 5
        self.assertEqual(
            match_setting_issues(first_match, "production"),
            (),
        )
        first_match["map"]["width"] = 24
        self.assertEqual(
            match_setting_issues(first_match, "production"),
            ("マップサイズ",),
        )
        rehearsal["jammedThreshold"] = 3
        self.assertEqual(
            match_setting_issues(rehearsal, "rehearsal"),
            ("渋滞基準値",),
        )

    def test_spoken_checkpoint_uses_selected_target_day(self) -> None:
        self.assertEqual(
            parse_spoken_checkpoint("15 42 12", 2),
            {
                "nextDay": 2,
                "matchBrands": 15,
                "totalUdon": 42,
                "sumOfDailyBrandCounts": 12,
            },
        )
        for text, next_day in (
            ("15 42", 2),
            ("15 x 12", 2),
            ("-1 42 12", 2),
            ("15 42 12", 0),
        ):
            with self.assertRaises(WorkflowError):
                parse_spoken_checkpoint(text, next_day)

        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            workflow = Workflow(
                ConnectionConfig("http://example", "secret"), ROOT / "main", work,
            )
            output = workflow.import_spoken_checkpoint("15 42 12", 2)
            self.assertEqual(output, (work / "score-day2.json").resolve())
            self.assertEqual(read_json(output)["nextDay"], 2)

    def test_compact_clock_status(self) -> None:
        values = {"phase": "開始前（エージェント選択）", "progress": "0/8日"}
        self.assertEqual(
            compact_clock_status(values, "自動: エージェント種別選択 / day=1"),
            "0/8日 エージェント選択",
        )
        values = {"phase": "3日目", "progress": "3/8日"}
        self.assertEqual(
            compact_clock_status(values, "自動: 探索 / day=3"),
            "3/8日 探索中",
        )
        self.assertEqual(compact_clock_status(values, "未実行"), "3/8日 待機中")
        self.assertTrue(automatic_phase_is_waiting(
            "自動: 問題情報の公開待ち・取得 / day=1"
        ))
        self.assertTrue(automatic_phase_is_waiting(
            "自動: 当日マップの公開待ち・取得 / day=3"
        ))
        self.assertFalse(automatic_phase_is_waiting("自動: 探索 / day=3"))
        self.assertEqual(
            compact_clock_status(
                values, "自動: 当日マップの公開待ち・取得 / day=3"
            ),
            "3/8日 待機中",
        )

    def test_waiting_resubmission_uses_previous_accepted_day(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            workflow = Workflow(
                ConnectionConfig("http://example", "test"), ROOT / "main", work
            )
            setting = work / "setting.json"
            for path, value in (
                (setting, {}),
                (work / "answer.json", [[]]),
                (work / "score-next.json", {"nextDay": 2}),
                (work / "state-day1.json", {"day": 1}),
                (work / "prepared-answer.json", {"day": 1}),
                (work / "submission-day1.json", {"status": "accepted"}),
            ):
                path.write_text(json.dumps(value), encoding="utf-8")
            gui = SolverGUI.__new__(SolverGUI)
            gui._busy = True
            gui._submission_busy = False
            gui._automatic_phase = "自動: 当日マップの公開待ち・取得 / day=3"
            gui._automatic_workflow = workflow
            gui._automatic_runner = SimpleNamespace(
                phase="state", day=2, setting=setting
            )

            candidate = gui._waiting_resubmission_candidate()
            self.assertIsNotNone(candidate)
            self.assertEqual(candidate[1], 1)
            self.assertEqual(candidate[4], (work / "state-day1.json").resolve())

            gui._automatic_runner.phase = "solve"
            self.assertIsNone(gui._waiting_resubmission_candidate())

    def test_match_clock_values(self) -> None:
        self.assertEqual(duration_text(3661.9), "61:01")
        before = match_clock_values(1000, (60, 90), 970, 990)
        self.assertEqual(before["phase"], "開始前（エージェント選択）")
        self.assertEqual(before["selection"], "残り 00:20")
        self.assertIn("開始まで 00:30", before["match"])
        self.assertEqual(before["timer"], "00:20")

        waiting = match_clock_values(1000, (60, 90), 995, 990)
        self.assertEqual(waiting["phase"], "開始前（試合待機）")
        self.assertEqual(waiting["timer"], "00:05")

        first = match_clock_values(1000, (60, 90), 1025, 990)
        self.assertEqual(first["phase"], "1日目")
        self.assertEqual(first["progress"], "1/2日")
        self.assertIn("残り 00:35", first["day"])
        self.assertEqual(first["timer"], "00:35")

        second = match_clock_values(1000, (60, 90), 1080, 990)
        self.assertEqual(second["phase"], "2日目")
        self.assertEqual(second["progress"], "2/2日")
        self.assertIn("経過 00:20", second["day"])

        finished = match_clock_values(1000, (60, 90), 1160, 990)
        self.assertEqual(finished["phase"], "全日程終了")
        self.assertEqual(finished["day"], "終了後 00:10")
        self.assertEqual(finished["timer"], "00:10")

        unknown = match_clock_values(0, (60, 60), 100)
        self.assertEqual(unknown["phase"], "時刻情報待ち")
        self.assertEqual(unknown["match"], "予定 02:00")
        self.assertEqual(unknown["timer"], "--:--")

        deadline_only = match_clock_values(
            0, (60, 90), 1025, current_day=1, day_ends_at=1090
        )
        self.assertEqual(deadline_only["phase"], "2日目")
        self.assertEqual(deadline_only["progress"], "2/2日")
        self.assertIn("残り 01:05", deadline_only["day"])
        self.assertEqual(deadline_only["timer"], "01:05")

        deadline_passed = match_clock_values(
            0, (60, 90), 1100, current_day=1, day_ends_at=1090
        )
        self.assertEqual(deadline_passed["phase"], "2日目終了")
        self.assertEqual(deadline_passed["timer"], "00:10")

    def test_unknown_setting_keeps_simple_server_clock(self) -> None:
        gui = SimpleNamespace(
            _clock_starts_at=1000.0,
            _clock_selection_ends_at=990.0,
            _clock_day_seconds=(),
            _milestones={"match-start": (1000.0, "試合開始")},
        )
        SolverGUI._set_setting_milestones(gui, 0, (60, 90))
        self.assertEqual(gui._clock_starts_at, 1000.0)
        self.assertEqual(gui._clock_selection_ends_at, 990.0)
        self.assertEqual(gui._clock_day_seconds, (60, 90))
        self.assertIn("match-start", gui._milestones)

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
            (16, 16, 4, 16, 14, 10),
        )
        for size, agents, low, high in ((16, 4, 10, 14),
                                        (24, 5, 14, 20),
                                        (32, 7, 20, 28)):
            for brands in (low, high):
                parameters = RandomSettingParameters(
                    width=size, height=size, agents=agents, spots=size,
                    brands=brands, days=4, teams=2, seed=99,
                )
                self.assertEqual(parameters.arguments()[-2:], ["--seed", "99"])
            with self.assertRaisesRegex(WorkflowError, "brands"):
                RandomSettingParameters(
                    width=size, height=size, agents=agents, spots=size,
                    brands=low - 1,
                ).arguments()
        with self.assertRaisesRegex(WorkflowError, "width/height"):
            RandomSettingParameters(width=16, height=24).arguments()
        with self.assertRaisesRegex(WorkflowError, "agents"):
            RandomSettingParameters(agents=8).arguments()
        with self.assertRaisesRegex(WorkflowError, "spots"):
            RandomSettingParameters(spots=24).arguments()
        with self.assertRaisesRegex(WorkflowError, "seconds"):
            RandomSettingParameters(seconds=59).arguments()
        with self.assertRaisesRegex(WorkflowError, "include_congestion"):
            RandomSettingParameters(include_congestion=2).arguments()
        self.assertNotIn(
            "--include_congestion",
            RandomSettingParameters(include_congestion=1).arguments(),
        )

    def test_day_time_limit_ms(self) -> None:
        self.assertEqual(SearchParameters().time_limit_ms, 20000)
        self.assertEqual(SearchParameters().safety_margin_ms, 5000)
        self.assertEqual(SearchParameters().send_reserve_ms, 5000)
        self.assertEqual(SearchParameters().trial_candidates, 6)
        self.assertEqual(SearchParameters().selection_time_ms, 20000)
        self.assertIn("--auto-beam-width", SearchParameters(auto_beam_width=1).arguments())
        self.assertIn("--udon-sa", SearchParameters(udon_sa=0).arguments())
        self.assertIn("--stable-beam-stop", SearchParameters(
            stable_beam_stop=1).arguments())
        self.assertIn("--sa-stagnation-stop", SearchParameters(
            sa_stagnation_stop=1).arguments())
        with self.assertRaisesRegex(WorkflowError, "auto_beam_width"):
            SearchParameters(auto_beam_width=2).arguments()
        with self.assertRaisesRegex(WorkflowError, "udon_sa"):
            SearchParameters(udon_sa=2).arguments()
        with self.assertRaisesRegex(WorkflowError, "stable_beam_stop"):
            SearchParameters(stable_beam_stop=2).arguments()
        with self.assertRaisesRegex(WorkflowError, "sa_stagnation_stop"):
            SearchParameters(sa_stagnation_stop=2).arguments()
        setting = {"daySeconds": [60, 12.5, 3_000_000]}
        self.assertEqual(day_time_limit_ms(setting, 0), 60000)
        self.assertEqual(day_time_limit_ms(setting, 1), 12500)
        self.assertEqual(day_time_limit_ms(setting, 2), 2147483647)
        with self.assertRaisesRegex(WorkflowError, "当日の試合時間"):
            day_time_limit_ms(setting, 3)

    def test_gui_build_uses_release_optimization(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            solver = work / "main"
            workflow = Workflow(
                ConnectionConfig("", ""), solver, work,
            )
            completed = mock.Mock(returncode=0, stdout="", stderr="")
            with mock.patch("backend.subprocess.run", return_value=completed) as run:
                result = workflow.build_solver()
            command = result.command
            self.assertIn("-O3", command)
            self.assertIn("-DNDEBUG", command)
            self.assertIn("-Werror", command)
            self.assertNotIn("-O2", command)
            run.assert_called_once()

    def test_agent_selection_always_reserves_submission_time(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            setting = work / "setting.json"
            agents = work / "agents.json"
            setting.write_text(json.dumps({"startsAt": 0}), encoding="utf-8")
            agents.write_text("[0, 0, 0, 1]", encoding="utf-8")
            workflow = Workflow(
                ConnectionConfig("http://example", "secret"),
                Path(sys.executable), work,
            )
            completed = mock.Mock(output="")
            started = time.time()
            with mock.patch.object(workflow, "_run", return_value=completed) as run:
                self.assertIs(workflow.select_agents(setting, agents), completed)
            self.assertTrue(run.call_args.kwargs["reserve_for_submission"])
            self.assertGreater(run.call_args.kwargs["ends_at"], started)
            self.assertLessEqual(run.call_args.kwargs["ends_at"], started + 61)

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
            self.assertEqual(config.timeout, DEFAULT_TIMEOUT_SECONDS)
            self.assertEqual(config.timeout, 10.0)

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

            # 3台のうちこの候補が最良だった場合、他端末の送信後にも
            # 受理済みの同一回答を明示的に再POSTできる。
            with mock.patch("backend.send", return_value=5) as post:
                revision, _, _ = workflow.send_answer(
                    0, answer, score, force=True
                )
                self.assertEqual(revision, 5)
                post.assert_called_once()
            self.assertEqual(read_json(work / "submission-day0.json")["revision"], 5)

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

    def test_dummy_connection_test_posts_agent_kinds_and_all_days(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workflow = Workflow(
                ConnectionConfig("http://example", "typed-token"),
                ROOT / "main", Path(directory),
            )
            responses = [
                ({"map": {}, "agents": [10, 20, 30, 40],
                  "daySteps": [3, 4, 5, 6],
                  "daySeconds": [20] * 4}, b"{}", 200),
                (None, b"", 200),
                ({"day": 0}, b"{}", 200),
                ({"revision": 7}, b"{}", 200),
                ({"day": 1}, b"{}", 200),
                ({"revision": 8}, b"{}", 200),
                ({"day": 2}, b"{}", 200),
                ({"revision": 9}, b"{}", 200),
                ({"day": 3}, b"{}", 200),
                ({"revision": 10}, b"{}", 200),
            ]
            with mock.patch("backend.request_json", side_effect=responses) as request:
                result = workflow.run_dummy_connection_test()
            self.assertIn("全4日ダミー回答受理", result)
            self.assertIn("revision=7,8,9,10", result)
            self.assertEqual(request.call_count, 10)
            self.assertEqual(request.call_args_list[0].args[2], "/setting")
            self.assertEqual(request.call_args_list[1].args[2], "/agent")
            self.assertEqual(request.call_args_list[1].kwargs["method"], "POST")
            self.assertEqual(request.call_args_list[1].kwargs["payload"], [0, 1, 0, 1])
            self.assertEqual(request.call_args_list[3].kwargs["payload"], [[-3]] * 4)
            self.assertEqual(request.call_args_list[5].kwargs["payload"], [[-4]] * 4)
            self.assertEqual(request.call_args_list[7].kwargs["payload"], [[-5]] * 4)
            self.assertEqual(request.call_args_list[9].kwargs["payload"], [[-6]] * 4)
            self.assertEqual(list(Path(directory).iterdir()), [])

            with mock.patch(
                "backend.request_json", side_effect=ApiError(403, "not ready")
            ):
                with self.assertRaisesRegex(WorkflowError, "ダミーデータ送信失敗"):
                    workflow.run_dummy_connection_test(setting_wait_seconds=0)

            before_public = [
                ApiError(403, "not ready"),
                ({"agents": [1], "daySteps": [1] * 4,
                  "daySeconds": [19] * 4}, b"{}", 200),
            ]
            with mock.patch("backend.request_json", side_effect=before_public) as request, \
                    mock.patch("backend.time.sleep") as sleep:
                with self.assertRaisesRegex(WorkflowError, "20秒"):
                    workflow.run_dummy_connection_test(setting_wait_seconds=1)
                self.assertEqual(request.call_count, 2)
                sleep.assert_called_once_with(0.2)

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
                result_states = [
                    Path(value) for kind, value in events
                    if kind == "result_state"
                ]
                self.assertEqual(
                    [path.resolve() for path in result_states],
                    [(work / "result-state.json").resolve()] * 2,
                )
                for index, (kind, _value) in enumerate(events):
                    if kind == "result_state":
                        self.assertEqual(events[index - 1][0], "answer")
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
    def test_generator_rejects_non_competition_maps(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "invalid.json"
            solver = os.environ["PROCON_TEST_SOLVER"]
            for size, agents, brands in ((16, 4, 10), (24, 5, 14),
                                         (32, 7, 28)):
                result = subprocess.run(
                    (solver, "generate", str(output),
                     "--width", str(size), "--height", str(size),
                     "--agents", str(agents), "--spots", str(size),
                     "--brands", str(brands)),
                    text=True, capture_output=True, check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                setting = read_json(output)
                self.assertEqual(len(setting["agents"]), agents)
                self.assertEqual(len(setting["spots"]), size)
                self.assertEqual(len({spot["brand"] for spot in setting["spots"]}), brands)
                output.unlink()
            for options in (
                ("--width", "8", "--height", "8"),
                ("--width", "24", "--height", "24"),
                ("--agents", "8"),
                ("--spots", "24"),
                ("--brands", "29"),
                ("--seconds", "59"),
            ):
                result = subprocess.run(
                    (solver, "generate", str(output), *options),
                    text=True, capture_output=True, check=False,
                )
                self.assertNotEqual(result.returncode, 0, options)
                self.assertFalse(output.exists(), options)

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
            with mock.patch("backend.secrets.randbits", return_value=123):
                workflow.generate_random_setting(
                    output,
                    RandomSettingParameters(
                        width=16, height=16, agents=4, spots=16, brands=10,
                        days=4, seconds=60, teams=3, seed=999,
                    ),
                )
            setting = read_json(output)
            self.assertEqual((setting["map"]["width"], setting["map"]["height"]), (16, 16))
            self.assertEqual(len(setting["agents"]), 4)
            self.assertEqual(len(setting["spots"]), 16)
            self.assertEqual(len({spot["brand"] for spot in setting["spots"]}), 10)
            self.assertEqual(len(setting["daySteps"]), 4)
            self.assertEqual(setting["daySeconds"], [60] * 4)
            self.assertEqual(setting["players"], 3)
            workflow.parameters = SearchParameters(
                beam_width=8, joint_beam_width=32, threads=1,
                time_limit_ms=100, safety_margin_ms=0, trial_candidates=6,
            )
            for include_congestion in (0, 1):
                parameters = RandomSettingParameters(
                    width=16, height=16, agents=4, spots=16, brands=10,
                    days=4, seconds=60, teams=3, seed=123,
                    include_congestion=include_congestion,
                )
                with mock.patch("backend.secrets.randbits", return_value=123):
                    workflow.generate_random_setting(output, parameters)
                generated = read_json(output)
                self.assertEqual(generated["map"], setting["map"])
                traffic = generated["localTraffics"]
                allowed = {0, 1, 2} if include_congestion else {0}
                self.assertEqual({t["status"] for t in traffic}, allowed)
                self.assertEqual({t["pos"] for t in traffic}, {
                    y * 16 + x for y, row in enumerate(generated["map"]["cells"])
                    for x, cell in enumerate(row) if cell == 1
                })
                with mock.patch("backend.secrets.randbits", return_value=123):
                    workflow.generate_random_setting(output, parameters)
                self.assertEqual(read_json(output)["localTraffics"], traffic)
                result = workflow.solve_local(output)
                self.assertIn(str((work / "local-state.json").resolve()), result.command)
                self.assertEqual(read_json(work / "local-state.json")["traffics"], traffic)
                self.assertEqual(read_json(work / "result-state.json")["traffics"], traffic)
                snapshot = load_snapshot(output, work / "result-state.json")
                self.assertEqual(dict(snapshot.traffics), {t["pos"]: t["status"] for t in traffic})

    @unittest.skipUnless(os.environ.get("PROCON_TEST_SOLVER"), "PROCON_TEST_SOLVERを指定するとC++連携も検査")
    def test_visualizer_generation_uses_fresh_seed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = work / "random-setting.json"
            workflow = Workflow(ConnectionConfig("", ""),
                                Path(os.environ["PROCON_TEST_SOLVER"]), work)
            parameters = RandomSettingParameters(
                width=16, height=16, agents=4, spots=16, brands=10,
                days=4, seed=42, include_congestion=1,
            )
            with mock.patch("backend.secrets.randbits", side_effect=(123, 124)):
                first = workflow.generate_random_setting(output, parameters)
                first_setting = read_json(output)
                second = workflow.generate_random_setting(output, parameters)
                second_setting = read_json(output)
            self.assertEqual(first.command[-2:], ("--seed", "123"))
            self.assertEqual(second.command[-2:], ("--seed", "124"))
            self.assertNotEqual(first_setting["map"], second_setting["map"])

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
            completed = workflow.solve_local(setting)
            self.assertIn("day-beam=1", completed.output)
            # GUIを経由しないCLIでも、探索時間0になる設定を拒否する。
            rejected = subprocess.run(
                [os.environ["PROCON_TEST_SOLVER"], str(setting),
                 str(work / "local-state.json"), "--time-limit-ms", "1000",
                 "--safety-margin-ms", "5000"],
                cwd=work, capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("time limit must exceed safety margin", rejected.stderr)
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
