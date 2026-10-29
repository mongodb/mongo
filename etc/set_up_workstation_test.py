from __future__ import annotations

import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).with_name("set_up_workstation.sh")


class SetUpWorkstationTest(unittest.TestCase):
    def test_python_environment_is_ready_before_bazel_runs(self) -> None:
        script = SCRIPT.read_text(encoding="utf-8")
        run_setup = script[script.index("run_setup() {") :]

        self.assertLess(run_setup.index("setup_pipx"), run_setup.index("setup_clang_config"))
        self.assertLess(run_setup.index("setup_uv"), run_setup.index("setup_clang_config"))
        self.assertLess(run_setup.index("setup_mongo_venv"), run_setup.index("setup_clang_config"))
        self.assertEqual(1, run_setup.count("setup_clang_config"))


if __name__ == "__main__":
    unittest.main()
