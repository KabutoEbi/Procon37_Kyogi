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

from app import SolverGUI
from backend import ConnectionConfig, CommandResult, SimpleServerInfo


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
            self.assertTrue(gui.winfo_viewable())
            self.assertTrue(gui.map_canvas.winfo_ismapped())
            self.assertGreater(gui.map_canvas.winfo_width(), 100)
            self.assertTrue(gui.settings_button.winfo_ismapped())
            self.assertFalse(gui.settings_window.winfo_viewable())
            gui._show_settings_window()
            gui.update()
            self.assertTrue(gui.settings_window.winfo_viewable())
            gui._hide_settings_window()
            gui.update()
            self.assertFalse(gui.settings_window.winfo_viewable())
            self.assertEqual(gui.token_entry.cget("show"), "")
            self.assertTrue(gui.simple_toggle_check.winfo_ismapped())
            self.assertFalse(gui.simple_frame.winfo_ismapped())
            self.assertNotEqual(gui.controls_frame.master.winfo_class(), "Canvas")
            self.assertNotEqual(gui.parameters_frame.master.winfo_class(), "Canvas")
            self.assertEqual(gui.files_frame.master, gui.config_frame)
            self.assertEqual(gui.log_frame.master, gui.content_pane)
            self.assertEqual(gui.controls_frame.master, gui.control_panel)
            self.assertLess(gui.control_panel.winfo_width(), 320)
            self.assertEqual(gui.agent_table.master.master, gui.map_state_panel)
            self.assertEqual(gui.spot_table.master.master, gui.map_state_panel)
            gui.work_var.set(directory)
            gui.solver_var.set(sys.executable)
            gui.parameter_vars["beam_width"].set("19")
            gui.parameter_vars["joint_beam_width"].set("71")
            gui.parameter_vars["trial_candidates"].set("0")
            callback_errors = []
            gui.report_callback_exception = lambda *args: callback_errors.append(args)
            main_thread = threading.get_ident()
            setting = json.loads((ROOT / "sample_maps/sample_16.json").read_text())
            setting["daySteps"] = setting["daySteps"][:4]
            posted = []
            day = 0

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
                    (workflow.work_dir / "answer.json").write_text("[[2,-31],[2,-31],[-32],[-32]]")
                    (workflow.work_dir / "score-next.json").write_text(json.dumps({"nextDay": day + 1}))
                return CommandResult((), "test-token log", "")

            def send(_url, token, kind, _path, **_kwargs):
                nonlocal day
                self.assertEqual(token, "entered-token")
                posted.append(kind)
                if kind == "action":
                    day += 1
                    return day
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
                with mock.patch.object(
                    gui._simple_server,
                    "start",
                    return_value=SimpleServerInfo(
                        "http://127.0.0.1:18080", "local-token", "local-team",
                        Path(directory) / "simple-server.log",
                    ),
                ) as start:
                    gui._start_simple_server()
                    drain()
                    start.assert_called_once()
                self.assertEqual(gui.url_var.get(), "http://127.0.0.1:18080")
                self.assertEqual(gui.token_var.get(), "local-token")
                self.assertIn("runtime/simple-server/client", gui.work_var.get())
                gui.simple_toggle_check.invoke()
                gui.update()
                self.assertFalse(gui.simple_frame.winfo_ismapped())

                gui.work_var.set(directory)
                gui.token_var.set("entered-token")
                with mock.patch("backend.request_json", return_value=({"map": {}}, b"{}", 200)) as probe:
                    self.assertEqual(probe.call_count, 0)
                    gui._check_connection()
                    drain()
                    self.assertEqual(probe.call_args.args[1], "entered-token")

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
                self.assertEqual(gui.day_var.get(), 0)
                self.assertEqual(int(gui.day_spinbox.cget("to")), 3)

                with mock.patch("backend.fetch", side_effect=fetch), \
                     mock.patch("backend.Workflow._run", autospec=True, side_effect=solver):
                    gui._fetch_and_solve_day()
                    drain()
                self.assertEqual(posted, ["agent"])
                self.assertTrue((Path(directory) / "state-day0.json").is_file())
                self.assertTrue((Path(directory) / "answer.json").is_file())
                self.assertEqual(gui._current_snapshot.day, 0)
                self.assertTrue(gui.map_canvas.find_withtag("answer-route"))
                first_route = gui.map_canvas.find_withtag("answer-route-agent-0")
                second_route = gui.map_canvas.find_withtag("answer-route-agent-1")
                self.assertTrue(first_route and second_route)
                self.assertNotEqual(
                    gui.map_canvas.itemcget(first_route[0], "fill"),
                    gui.map_canvas.itemcget(second_route[0], "fill"),
                )
                with mock.patch("app.messagebox.askyesno", return_value=True), \
                     mock.patch("backend.send", side_effect=send):
                    gui._send_answer()
                    drain(expected_posts=2)
                self.assertEqual(gui.day_var.get(), 1)
                self.assertTrue(gui.map_canvas.find_withtag("answer-route"))
                gui.mode_var.set("auto")
                gui._mode_changed()
                self.assertEqual(str(gui.send_answer_button["state"]), "disabled")
                with mock.patch("backend.fetch", side_effect=fetch), \
                     mock.patch("backend.Workflow._run", autospec=True, side_effect=solver), \
                     mock.patch("backend.send", side_effect=send):
                    gui._fetch_and_solve_day()
                    drain(expected_posts=3)
                self.assertEqual(posted, ["agent", "action", "action"])
                self.assertEqual(gui.day_var.get(), 2)
                self.assertIn("score-day2.json", gui.score_var.get())
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
            finally:
                gui.destroy()


if __name__ == "__main__":
    unittest.main()
