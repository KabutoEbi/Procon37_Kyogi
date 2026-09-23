"""ウィンドウを作らずにGUI起動環境の選択を検査する。"""

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "visualizer"))
import runtime


class RuntimeTest(unittest.TestCase):
    def test_supported_python_is_kept(self):
        with mock.patch("runtime.supported_tk", return_value=True):
            self.assertEqual(runtime.find_runtime(), sys.executable)

    def test_old_tk_selects_working_candidate(self):
        with mock.patch("runtime.supported_tk", return_value=False), \
             mock.patch("runtime.sys.platform", "darwin"), \
             mock.patch("runtime.candidates", return_value=iter(["bad-python", "good-python"])), \
             mock.patch("runtime.subprocess.run", side_effect=[
                 mock.Mock(returncode=1), mock.Mock(returncode=0),
             ]):
            self.assertEqual(runtime.find_runtime(), "good-python")

    def test_no_compatible_runtime_reports_actionable_error(self):
        with mock.patch("runtime.supported_tk", return_value=False), \
             mock.patch("runtime.sys.platform", "darwin"), \
             mock.patch("runtime.candidates", return_value=iter([])):
            with self.assertRaisesRegex(RuntimeError, "Tk 8.6"):
                runtime.find_runtime()


if __name__ == "__main__":
    unittest.main()
