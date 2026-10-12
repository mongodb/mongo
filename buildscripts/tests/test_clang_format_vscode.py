"""Exercise the editor formatter wrapper without downloading a toolchain."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


# resmoke's buildscripts_test suite runs this module directly, bypassing the
# Bazel target's Windows restriction. These fixtures assume POSIX paths and permissions.
@unittest.skipIf(sys.platform == "win32", "clang-format editor wrapper tests require POSIX")
class TestClangFormatVSCode(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.root = Path(self.tempdir.name)
        self.workspace = self.root / "workspace with spaces"
        scripts = self.workspace / "buildscripts"
        scripts.mkdir(parents=True)
        self.wrapper = scripts / "clang_format_vscode.sh"
        shutil.copy2(Path(__file__).parents[1] / self.wrapper.name, self.wrapper)
        self.caller = self.workspace / "src"
        self.caller.mkdir()
        self.repo = self.root / "output base:custom" / "external" / "+extension+toolchain"
        self.bin_dir = self.root / "bin"
        self.bin_dir.mkdir()
        bazel = self.bin_dir / "bazel"
        bazel.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$PWD" "$@" > "$BAZEL_CALL"
# A setup command must not consume the editor's input.
cat > "$BAZEL_INPUT"
echo 'Bazel diagnostic' >&2
if [[ "$QUERY_STATUS" != 0 ]]; then
    exit "$QUERY_STATUS"
fi
if [[ "$BAD_LOCATION" == 1 ]]; then
    echo 'unexpected query output'
    exit 0
fi
mkdir -p "$TOOLCHAIN_REPO/v5/bin"
if [[ "$FORMATTER_MODE" != missing ]]; then
    cp "$FORMATTER_TEMPLATE" "$TOOLCHAIN_REPO/v5/bin/clang-format"
    chmod "$FORMATTER_MODE" "$TOOLCHAIN_REPO/v5/bin/clang-format"
fi
printf '%s:491:1: filegroup rule @mongo_toolchain_v5//:clang_format\\n' "$TOOLCHAIN_REPO/BUILD.bazel"
"""
        )
        bazel.chmod(0o755)
        template = self.root / "formatter"
        template.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$PWD" "$@"
cat
if [[ -n "$FORMATTER_STDERR" ]]; then
    printf '%s\\n' "$FORMATTER_STDERR" >&2
fi
exit "$FORMATTER_STATUS"
"""
        )
        self.env = {
            **os.environ,
            "PATH": f"{self.bin_dir}{os.pathsep}{os.environ['PATH']}",
            "TOOLCHAIN_REPO": str(self.repo),
            "FORMATTER_TEMPLATE": str(template),
            "FORMATTER_STATUS": "0",
            "FORMATTER_STDERR": "",
            "FORMATTER_MODE": "755",
            "QUERY_STATUS": "0",
            "BAD_LOCATION": "0",
            "BAZEL_CALL": str(self.root / "bazel_call"),
            "BAZEL_INPUT": str(self.root / "bazel_input"),
        }

    def run_wrapper(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(self.wrapper), *args],
            cwd=self.caller,
            env=self.env,
            input="int main() {}\n",
            text=True,
            capture_output=True,
            check=False,
        )

    def test_fetches_missing_toolchain_and_preserves_editor_io(self) -> None:
        self.assertFalse(self.repo.exists())
        result = self.run_wrapper("--assume-filename=source with spaces.cpp", "--style=file")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            f"{self.caller}\n--assume-filename=source with spaces.cpp\n--style=file\nint main() {{}}\n",
        )
        self.assertEqual(result.stderr, "")
        self.assertEqual(Path(self.env["BAZEL_INPUT"]).read_text(), "")
        self.assertEqual(
            Path(self.env["BAZEL_CALL"]).read_text().splitlines()[0], str(self.workspace)
        )
        self.assertTrue((self.repo / "v5/bin/clang-format").is_file())

    def test_propagates_formatter_exit_status(self) -> None:
        self.env["FORMATTER_STATUS"] = "23"
        self.assertEqual(self.run_wrapper("--version").returncode, 23)

    def test_preserves_formatter_diagnostics(self) -> None:
        self.env["FORMATTER_STDERR"] = "clang-format diagnostic"
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, "clang-format diagnostic\n")

    def test_propagates_bazel_failure_without_formatted_output(self) -> None:
        self.env["QUERY_STATUS"] = "42"
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 42)
        self.assertEqual(result.stdout, "")
        self.assertIn("Bazel diagnostic", result.stderr)

    def test_resolves_current_toolchain_on_each_invocation(self) -> None:
        self.assertEqual(self.run_wrapper().returncode, 0)
        self.env["TOOLCHAIN_REPO"] = str(self.root / "another output base" / "external" / "repo")
        self.assertEqual(self.run_wrapper().returncode, 0)
        self.assertTrue((Path(self.env["TOOLCHAIN_REPO"]) / "v5/bin/clang-format").is_file())

    def test_rejects_missing_executable(self) -> None:
        self.env["FORMATTER_MODE"] = "missing"
        result = self.run_wrapper()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("not executable", result.stderr)

    def test_rejects_nonexecutable_formatter(self) -> None:
        self.env["FORMATTER_MODE"] = "644"
        result = self.run_wrapper()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("not executable", result.stderr)

    def test_rejects_unparseable_query_output(self) -> None:
        self.env["BAD_LOCATION"] = "1"
        result = self.run_wrapper()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("Could not locate", result.stderr)


if __name__ == "__main__":
    unittest.main()
