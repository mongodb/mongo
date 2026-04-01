"""Test that resmoke can be invoked via 'bazel run //buildscripts:resmoke'."""

import subprocess
import sys
import unittest


class TestBazelRunResmoke(unittest.TestCase):
    @unittest.skipIf(
        sys.platform == "win32",
        "TODO(SERVER-123249), enable this test once the win32api dependency is handled properly.",
    )
    def test_dry_run_core_suite(self):
        """Verify 'bazel run //buildscripts:resmoke -- run --suite=core --dryRun=tests' succeeds."""
        result = subprocess.run(
            [
                "bazel",
                "run",
                "--config=local",
                "//buildscripts:resmoke",
                "--",
                "run",
                "--suite=core",
                "--dryRun=tests",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(
            0,
            result.returncode,
            msg=(
                "bazel run //buildscripts:resmoke -- run --suite=core --dryRun=tests failed.\n\n"
                f"stdout:\n{result.stdout}\n\nstderr:\n{result.stderr}"
            ),
        )
        self.assertTrue(
            any(line.endswith(".js") for line in result.stdout.splitlines()),
            msg=f"Expected JS test file paths in dry-run output but got:\n{result.stdout}",
        )


if __name__ == "__main__":
    unittest.main()
