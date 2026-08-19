"""Static protocol checks for the platform Bazel wrapper scripts."""

from __future__ import annotations

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class WrapperScriptsProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bash = (REPO_ROOT / "tools" / "bazel").read_text(encoding="utf-8")
        cls.windows = (REPO_ROOT / "tools" / "bazel.bat").read_text(encoding="utf-8")

    def test_bash_skips_real_bazel_and_propagates_handled_exit(self):
        handled = self.bash.index('if [[ "$handled" == "1" ]]')
        propagated_exit = self.bash.index('exit "$handled_exit_code"', handled)
        normal_bazel = self.bash.index('"$bazel_real" "${new_args[@]}"', handled)
        self.assertLess(propagated_exit, normal_bazel)
        self.assertIn("MONGO_BAZEL_WRAPPER_STATUS", self.bash)
        self.assertIn('handled_exit_code=""', self.bash)

    def test_windows_captures_and_propagates_handled_exit(self):
        invocation = self.windows.index("wrapper_hook.py")
        capture = self.windows.index('set "exit_code=!ERRORLEVEL!"', invocation)
        handled = self.windows.index('if "!handled!"=="1"', capture)
        propagated_exit = self.windows.index("exit /b !quality_checks_exit!", handled)
        normal_bazel = self.windows.index('"%BAZEL_REAL%" !new_args!', handled)
        self.assertLess(invocation, capture)
        self.assertLess(propagated_exit, normal_bazel)
        self.assertIn("MONGO_BAZEL_WRAPPER_STATUS", self.windows)
        self.assertIn('set "handled_exit_code="', self.windows)
        self.assertIn('set "fallback_exit=!ERRORLEVEL!"', self.windows)


if __name__ == "__main__":
    unittest.main()
