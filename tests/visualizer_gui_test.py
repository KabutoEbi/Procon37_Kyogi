"""Tk操作テスト。デスクトップに接続できる環境で直接実行する。通信はモック。"""

import json
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "visualizer"))
from runtime import ensure_runtime

if __name__ == "__main__":
    ensure_runtime()

from app import MATCH_OPERATION_PROFILES, SolverGUI, TEAM_ROLE_LABELS
from backend import (
    ConnectionConfig,
    CommandResult,
    SimpleServerInfo,
    match_acquisition_theoretical,
)


class GUITest(unittest.TestCase):
    def test_buttons_auto_flow_and_parameter_snapshot(self):
        # 実際の.envを読まず、テスト用接続先だけを使用する。
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch("app.connection_from_env", return_value=ConnectionConfig("http://mock", "test-token")), \
             mock.patch("app.simple_server_teams", return_value=(("local-team", "local-token"),)), \
             mock.patch("app.messagebox.showerror") as error_dialog:
            gui = SolverGUI()
            # withdrawでは「部品は存在するが実画面は空白」を検出できない。
            gui.update()
            self.assertIsNotNone(gui._app_icon)
            self.assertTrue(gui.winfo_viewable())
            self.assertTrue(gui.map_canvas.winfo_ismapped())
            self.assertGreater(gui.map_canvas.winfo_width(), 100)
            self.assertTrue(gui.settings_button.winfo_ismapped())
            self.assertTrue(gui.simple_clear_button.winfo_ismapped())
            self.assertTrue(gui.automatic_match_button.winfo_ismapped())
            gui.team_role_var.set(TEAM_ROLE_LABELS["lead"])
            gui._team_role_changed()
            self.assertEqual(gui.parameter_vars["time_limit_ms"].get(), "10000")
            self.assertEqual(gui.parameter_vars["udon_sa"].get(), "0")
            self.assertEqual(gui.parameter_vars["stable_beam_stop"].get(), "0")
            self.assertEqual(gui.parameter_vars["sa_stagnation_stop"].get(), "0")
            self.assertIn("短時間ビーム", gui.build_solver_button.cget("text"))
            self.assertIsNone(gui._workflow().shared_dir)
            self.assertTrue(gui.spoken_checkpoint_entry.winfo_ismapped())
            self.assertTrue(gui.spoken_import_button.winfo_ismapped())
            self.assertEqual(gui.spoken_import_button.cget("text"), "取込")
            self.assertFalse(hasattr(gui, "spoken_show_button"))
            self.assertEqual(gui.spoken_result_var.get(), "- - -")
            self.assertEqual(str(gui.automatic_match_button["state"]), "normal")
            self.assertIn("担当1", gui.automatic_match_button.cget("text"))
            self.assertTrue(gui.prepare_button.cget("text").startswith("1b"))
            self.assertIn("接続テスト", gui.build_solver_button.cget("text"))
            gui.match_profile_var.set(MATCH_OPERATION_PROFILES["rehearsal"].label)
            gui._match_profile_changed()
            self.assertIn("予行演習", gui.build_solver_button.cget("text"))
            self.assertEqual(gui.parameter_vars["selection_time_ms"].get(), "30000")
            self.assertEqual(gui.parameter_vars["time_limit_ms"].get(), "10000")
            gui.match_profile_var.set(MATCH_OPERATION_PROFILES["production"].label)
            gui._match_profile_changed()
            self.assertIn("第1試合", gui.build_solver_button.cget("text"))
            self.assertEqual(gui.parameter_vars["selection_time_ms"].get(), "60000")
            self.assertEqual(gui.parameter_vars["time_limit_ms"].get(), "10000")
            gui.team_role_var.set(TEAM_ROLE_LABELS["middle"])
            gui._team_role_changed()
            self.assertEqual(gui.parameter_vars["time_limit_ms"].get(), "20000")
            self.assertEqual(gui.parameter_vars["udon_sa"].get(), "0")
            self.assertEqual(gui.parameter_vars["stable_beam_stop"].get(), "1")
            self.assertEqual(gui.parameter_vars["sa_stagnation_stop"].get(), "0")
            self.assertIn("収束ビーム", gui.build_solver_button.cget("text"))
            self.assertEqual(str(gui.automatic_match_button["state"]), "disabled")
            self.assertTrue(gui.prepare_button.cget("text").startswith("1a"))
            gui.team_role_var.set(TEAM_ROLE_LABELS["long"])
            gui._team_role_changed()
            self.assertEqual(gui.parameter_vars["time_limit_ms"].get(), "20000")
            self.assertEqual(gui.parameter_vars["udon_sa"].get(), "1")
            self.assertEqual(gui.parameter_vars["stable_beam_stop"].get(), "0")
            self.assertEqual(gui.parameter_vars["sa_stagnation_stop"].get(), "1")
            self.assertIn("収束SA", gui.build_solver_button.cget("text"))
            gui.team_role_var.set(TEAM_ROLE_LABELS["normal"])
            gui._team_role_changed()
            self.assertEqual(gui.parameter_vars["stable_beam_stop"].get(), "0")
            self.assertEqual(gui.parameter_vars["sa_stagnation_stop"].get(), "0")
            self.assertFalse(gui.settings_window.winfo_viewable())
            gui._show_settings_window()
            gui.update()
            self.assertTrue(gui.settings_window.winfo_viewable())
            self.assertTrue(gui.random_generate_button.winfo_ismapped())
            self.assertTrue(gui.random_traffic_check.winfo_ismapped())
            gui.random_traffic_check.invoke()
            parameters = gui._random_setting_parameters()
            self.assertEqual(parameters.include_congestion, 1)
            self.assertNotIn("seed", gui.random_setting_vars)
            gui.random_traffic_check.invoke()
            self.assertEqual(gui._random_setting_parameters().include_congestion, 0)
            gui._hide_settings_window()
            gui.update()
            self.assertFalse(gui.settings_window.winfo_viewable())
            self.assertEqual(gui.token_entry.cget("show"), "")
            self.assertTrue(gui.simple_toggle_check.winfo_ismapped())
            self.assertFalse(gui.simple_frame.winfo_ismapped())
            self.assertNotEqual(gui.controls_frame.master.winfo_class(), "Canvas")
            self.assertNotEqual(gui.parameters_frame.master.winfo_class(), "Canvas")
            self.assertEqual(gui.files_frame.master, gui.config_frame)
            self.assertEqual(gui.random_setting_frame.master, gui.config_frame)
            self.assertEqual(gui.log_frame.master, gui.content_pane)
            self.assertEqual(gui.controls_frame.master, gui.control_panel)
            self.assertGreaterEqual(gui.control_panel.winfo_width(), 300)
            self.assertLess(gui.control_panel.winfo_width(), 360)
            self.assertEqual(gui.search_result_frame.master, gui.controls_frame)
            self.assertGreater(
                int(gui.search_result_frame.grid_info()["row"]),
                int(gui.send_answer_button.grid_info()["row"]),
            )
            self.assertEqual(gui.agent_table.master.master, gui.map_state_panel)
            self.assertEqual(gui.spot_table.master.master, gui.map_state_panel)
            gui.work_var.set(directory)
            gui.solver_var.set(sys.executable)
            gui.parameter_vars["beam_width"].set("19")
            gui.parameter_vars["joint_beam_width"].set("71")
            self.assertEqual(gui.parameter_vars["auto_beam_width"].get(), "1")
            gui.parameter_vars["auto_beam_width"].set("0")
            gui.parameter_vars["trial_candidates"].set("0")
            callback_errors = []
            gui.report_callback_exception = lambda *args: callback_errors.append(args)
            main_thread = threading.get_ident()
            setting = json.loads((ROOT / "sample_maps/sample_16.json").read_text())
            setting["daySteps"] = setting["daySteps"][:4]
            setting["daySeconds"] = setting["daySeconds"][:4]
            setting["startsAt"] = 1_800_000_000
            unknown_schedule = dict(setting)
            unknown_schedule["startsAt"] = 0
            unknown_schedule_path = Path(directory) / "unknown-schedule.json"
            unknown_schedule_path.write_text(json.dumps(unknown_schedule))
            log_before_unknown_schedule = gui.log_text.get("1.0", "end")
            gui._log_match_schedule(unknown_schedule_path)
            self.assertEqual(
                gui.log_text.get("1.0", "end"),
                log_before_unknown_schedule,
            )
            posted = []
            day = 0
            answer_text = "[[2,-31],[2,-31],[-32],[-32]]"

            def fetch(_url, token, kind, path, **_kwargs):
                self.assertNotEqual(threading.get_ident(), main_thread)
                self.assertEqual(token, "entered-token")
                value = setting if kind == "setting" else {
                    "day": day, "endsAt": 0, "traffics": [],
                    "agents": [{"pos": pos, "kind": 0, "fuel": 40} for pos in setting["agents"]],
                }
                path.write_text(json.dumps(value))
                return value

            def solver(workflow, args, **_kwargs):
                self.assertNotEqual(threading.get_ident(), main_thread)
                self.assertEqual(workflow.parameters.beam_width, 19)
                self.assertEqual(workflow.parameters.joint_beam_width, 71)
                if args[0] == "select":
                    Path(args[2]).write_text("[0,0,0,0]")
                else:
                    (workflow.work_dir / "answer.json").write_text(answer_text)
                    (workflow.work_dir / "score-next.json").write_text(json.dumps({
                        "nextDay": day + 1,
                        "matchBrands": 15,
                        "sumOfDailyBrandCounts": 6,
                        "totalUdon": 1 + posted.count("action"),
                    }))
                    (workflow.work_dir / "result-state.json").write_text(json.dumps({
                        "day": day,
                        "step": setting["daySteps"][day],
                        "agents": [
                            {"kind": 0, "pos": pos, "fuel": 17}
                            for pos in setting["agents"]
                        ],
                        "remainingStocks": [0] * len(setting["spots"]),
                    }))
                report = f"""test-token log
=== procon37 solver report ===
day={day}
search
  success=true
  stop=time-limit
  elapsed-ms=1234
  expanded=56789
result
  udon=12
  daily-brands=3
  match-brands=4
    total-daily-brands=6
  total-udon=25
"""
                return CommandResult((), report, "")

            def send(_url, token, kind, _path, **_kwargs):
                self.assertEqual(token, "entered-token")
                posted.append(kind)
                if kind == "action":
                    return posted.count("action")
                return None

            def drain(expected_posts=None):
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    gui.update()
                    if not gui._busy and (expected_posts is None or len(posted) >= expected_posts):
                        break
                    time.sleep(0.01)
                gui.update()
                self.assertFalse(gui._busy)

            try:
                gui.simple_toggle_check.invoke()
                gui.update()
                self.assertTrue(gui.simple_start_button.winfo_ismapped())
                self.assertTrue(gui.simple_clear_button.winfo_ismapped())
                simple_now = time.time()
                with mock.patch.object(
                    gui._simple_server,
                    "start",
                    return_value=SimpleServerInfo(
                        "http://127.0.0.1:18080", "local-token", "local-team",
                        Path(directory) / "simple-server.log",
                        simple_now + 0.05,
                        simple_now + 0.10,
                        (0.05, 0.05),
                    ),
                ) as start:
                    gui._start_simple_server()
                    drain()
                    start.assert_called_once()
                self.assertEqual(gui.url_var.get(), "http://127.0.0.1:18080")
                self.assertEqual(gui.token_var.get(), "local-token")
                self.assertIn("runtime/simple-server/client", gui.work_var.get())
                milestone_deadline = time.monotonic() + 1
                milestone_log = ""
                while time.monotonic() < milestone_deadline:
                    gui.update()
                    milestone_log = gui.log_text.get("1.0", "end")
                    if "⏰ 2日目終了" in milestone_log:
                        break
                    time.sleep(0.01)
                self.assertIn("⏰ 選択時間終了", milestone_log)
                self.assertIn("⏰ 試合開始", milestone_log)
                self.assertIn("⏰ 1日目終了", milestone_log)
                self.assertIn("⏰ 2日目終了", milestone_log)
                with mock.patch("app.messagebox.askyesno", return_value=True), \
                     mock.patch.object(gui._simple_server, "clear_client_work", return_value=3) as clear:
                    gui.simple_clear_button.invoke()
                    clear.assert_called_once()
                    self.assertIn("クリアしました", gui.status_var.get())
                gui.simple_toggle_check.invoke()
                gui.update()
                self.assertFalse(gui.simple_frame.winfo_ismapped())
                self.assertTrue(gui.simple_clear_button.winfo_ismapped())

                gui.work_var.set(directory)
                gui.token_var.set("entered-token")
                probe_responses = [
                    ({"map": {}, "agents": [1, 2, 3, 4],
                      "daySteps": [1]}, b"{}", 200),
                    (None, b"", 200),
                    ({"day": 0}, b"{}", 200),
                    ({"revision": 0}, b"{}", 200),
                ]
                with mock.patch("app.messagebox.askyesno", return_value=True), \
                     mock.patch("backend.request_json", side_effect=probe_responses) as probe:
                    self.assertEqual(probe.call_count, 0)
                    gui._check_connection()
                    drain()
                    self.assertEqual(probe.call_count, 4)
                    self.assertEqual(probe.call_args.args[1], "entered-token")
                    self.assertEqual(probe.call_args.args[2], "/")
                    self.assertEqual(probe.call_args.kwargs["payload"], [[-1]] * 4)

                with mock.patch("backend.fetch", side_effect=fetch), \
                     mock.patch("backend.Workflow._run", autospec=True, side_effect=solver), \
                     mock.patch("backend.send", side_effect=send):
                    gui._prepare_match()
                    drain(expected_posts=1)
                self.assertEqual(posted, ["agent"])
                self.assertTrue((Path(directory) / "setting.json").is_file())
                self.assertTrue((Path(directory) / "agents.json").is_file())
                self.assertEqual(gui.setting_var.get(), str((Path(directory) / "setting.json").resolve()))
                self.assertEqual(gui._selected_kinds, [0, 0, 0, 0])
                self.assertEqual(gui.day_var.get(), 1)
                self.assertEqual(
                    gui.parameter_vars["time_limit_ms"].get(),
                    str(setting["daySeconds"][0] * 1000),
                )
                self.assertEqual(int(gui.day_spinbox.cget("to")), 4)
                self.assertIn("day=未取得", gui.map_info_var.get())
                self.assertIn("days=4", gui.map_info_var.get())
                self.assertIn(f"spots={len(setting['spots'])}", gui.map_info_var.get())
                self.assertIn(
                    f"brands={len({spot['brand'] for spot in setting['spots']})}",
                    gui.map_info_var.get(),
                )
                schedule_log = gui.log_text.get("1.0", "end")
                self.assertIn("試合開始予定", schedule_log)
                self.assertIn("1日目終了予定", schedule_log)
                self.assertIn("4日目終了予定", schedule_log)

                with mock.patch("backend.Workflow._run", autospec=True, side_effect=solver):
                    gui._solve_local()
                    drain()
                self.assertTrue(gui.state_var.get().endswith("result-state.json"))
                first_agent = gui.agent_table.item(gui.agent_table.get_children()[0], "values")
                first_spot = gui.spot_table.item(gui.spot_table.get_children()[0], "values")
                self.assertEqual(first_agent[1], "P")
                self.assertEqual(first_agent[3], "17/40")
                self.assertEqual(
                    first_agent[4],
                    f"{setting['daySteps'][0]}/{setting['daySteps'][0]}",
                )
                self.assertEqual(str(first_spot[3]), f"0/{setting['spots'][0]['stocks']}")
                self.assertTrue(gui.map_canvas.find_withtag("answer-route"))
                self.assertEqual(gui.search_result_vars["status"].get(), "時間上限")
                self.assertEqual(gui.search_result_vars["target"].get(), "ローカル day=1")
                self.assertEqual(gui.search_result_vars["elapsed"].get(), "1,234 ms")
                self.assertEqual(gui.search_result_vars["expanded"].get(), "56,789")
                total_udon = sum(spot["stocks"] for spot in setting["spots"])
                total_brands = len({spot["brand"] for spot in setting["spots"]})
                match_udon, match_brands = match_acquisition_theoretical(
                    Path(directory) / "setting.json", len(setting["agents"])
                )
                self.assertEqual(
                    gui.search_result_vars["daily_count"].get(), f"12/{total_udon}玉"
                )
                self.assertEqual(
                    gui.search_result_vars["total_count"].get(), f"25/{match_udon}玉"
                )
                self.assertEqual(
                    gui.search_result_vars["daily_brands"].get(), f"3/{total_brands}系列"
                )
                self.assertEqual(
                    gui.search_result_vars["total_brands"].get(), f"6/{match_brands}系列"
                )
                self.assertEqual(gui.match_score_var.get(), "4 / 6 / 25")
                self.assertEqual(
                    gui.log_text.get("1.0", "end").count("試合開始予定"),
                    1,
                )

                with mock.patch("backend.fetch", side_effect=fetch), \
                     mock.patch("backend.Workflow._run", autospec=True, side_effect=solver):
                    gui._fetch_and_solve_day()
                    drain()
                self.assertEqual(posted, ["agent"])
                self.assertTrue((Path(directory) / "state-day0.json").is_file())
                self.assertTrue((Path(directory) / "answer.json").is_file())
                self.assertTrue(gui.state_var.get().endswith("state-day0.json"))
                self.assertEqual(gui._current_snapshot.day, 0)
                first_spot = gui.spot_table.item(gui.spot_table.get_children()[0], "values")
                self.assertEqual(str(first_spot[3]), f"0/{setting['spots'][0]['stocks']}")
                self.assertTrue(gui.map_canvas.find_withtag("answer-route"))
                first_route = gui.map_canvas.find_withtag("answer-route-agent-0")
                second_route = gui.map_canvas.find_withtag("answer-route-agent-1")
                self.assertTrue(first_route and second_route)
                self.assertNotEqual(
                    gui.map_canvas.itemcget(first_route[0], "fill"),
                    gui.map_canvas.itemcget(second_route[0], "fill"),
                )
                spoken = gui.spoken_result_var.get()
                self.assertEqual(len(spoken.split()), 3)
                self.assertEqual(gui.spoken_checkpoint_var.get(), "")
                with mock.patch("app.messagebox.askyesno", return_value=True), \
                     mock.patch("backend.send", side_effect=send):
                    gui._send_answer()
                    drain(expected_posts=2)
                gui.day_var.set(2)
                gui.spoken_checkpoint_var.set(spoken)
                gui.spoken_checkpoint_entry.focus_force()
                gui.update()
                gui.spoken_checkpoint_entry.event_generate("<KeyPress-Return>")
                gui.update()
                self.assertEqual(
                    json.loads((Path(directory) / "score-day1.json").read_text())["nextDay"],
                    1,
                )
                self.assertEqual(gui.spoken_checkpoint_var.get(), "")
                gui.day_var.set(1)
                self.assertEqual(gui.day_var.get(), 1)
                self.assertEqual(gui.solve_day_button.cget("text"), "2  探索")
                # 同じ日の再探索はGETなし。自動モードでは新しい回答を送信する。
                gui.mode_var.set("auto")
                answer_text = "[[-32],[2,-31],[-32],[-32]]"
                with mock.patch("backend.fetch") as fetch_again, \
                     mock.patch("backend.Workflow._run", autospec=True, side_effect=solver), \
                     mock.patch("backend.send", side_effect=send):
                    gui.solve_day_button.invoke()
                    drain(expected_posts=3)
                    fetch_again.assert_not_called()
                self.assertEqual(gui.day_var.get(), 1)
                self.assertEqual(json.loads((Path(directory) / "submission-day0.json").read_text())["revision"], 2)
                # 終了時刻を過ぎたときだけ対象日を進め、初回取得ボタンに戻す。
                gui._day_deadlines[0] = time.time() - 1
                gui._advance_finished_day()
                day = 1
                self.assertEqual(gui.day_var.get(), 2)
                self.assertEqual(gui.solve_day_button.cget("text"), "2  状態公開待ち・取得・探索")
                self.assertTrue(gui.map_canvas.find_withtag("answer-route"))
                gui.mode_var.set("auto")
                gui._mode_changed()
                self.assertEqual(str(gui.send_answer_button["state"]), "disabled")
                with mock.patch("backend.fetch", side_effect=fetch), \
                     mock.patch("backend.Workflow._run", autospec=True, side_effect=solver), \
                     mock.patch("backend.send", side_effect=send):
                    gui._fetch_and_solve_day()
                    drain(expected_posts=4)
                self.assertEqual(posted, ["agent", "action", "action", "action"])
                self.assertEqual(gui.day_var.get(), 2)
                self.assertIn("score-day1.json", gui.score_var.get())
                self.assertEqual(gui.solve_day_button.cget("text"), "2  探索")
                self.assertEqual(gui._current_snapshot.day, 1)
                self.assertFalse((Path(directory) / "state-day2.json").exists())
                self.assertTrue(gui.map_canvas.find_withtag("answer-route"))
                self.assertEqual(len(gui.agent_table.get_children()), 4)
                self.assertEqual(
                    len(gui.spot_table.get_children()), len(setting["spots"])
                )
                self.assertGreater(len(gui.map_canvas.find_all()), 256)
                gui.update()
                self.assertFalse(gui.map_xbar.winfo_ismapped())
                self.assertFalse(gui.map_ybar.winfo_ismapped())
                self.assertNotIn("entered-token", gui.log_text.get("1.0", "end"))
                self.assertFalse(callback_errors)
                error_dialog.assert_not_called()
                gui.mode_var.set("manual")
                gui._mode_changed()
                self.assertEqual(str(gui.send_answer_button["state"]), "normal")
                deadline = gui._day_deadlines[1]
                with mock.patch("app.time.time", return_value=deadline - 0.01):
                    gui._advance_finished_day()
                    self.assertEqual(gui.day_var.get(), 2)
                with mock.patch("app.time.time", return_value=deadline):
                    gui._busy = True
                    gui._advance_finished_day()
                    self.assertEqual(gui.day_var.get(), 2)
                    gui._busy = False
                    gui._advance_finished_day()
                    self.assertEqual(gui.day_var.get(), 3)
                with mock.patch("app.time.time", return_value=gui._day_deadlines[3]):
                    gui._advance_finished_day()
                    self.assertEqual(gui.day_var.get(), 5)
                    self.assertEqual(str(gui.solve_day_button["state"]), "disabled")
                with mock.patch("app.messagebox.showinfo") as info_dialog:
                    gui.spoken_checkpoint_var.set("15 42 12")
                    gui.spoken_import_button.invoke()
                    gui.update()
                    info_dialog.assert_called_once()
                    self.assertIn("取込不要", gui.status_var.get())
                    self.assertFalse((Path(directory) / "score-day4.json").exists())
                gui.day_var.set(1)
                gui.url_var.set("http://another-match")
                self.assertFalse(gui._daily_inputs)
                self.assertFalse(gui._day_deadlines)
                self.assertEqual(gui.solve_day_button.cget("text"), "2  状態公開待ち・取得・探索")
            finally:
                gui.destroy()


if __name__ == "__main__":
    unittest.main()
