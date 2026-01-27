import json
import os
import pathlib
import runpy
import tempfile
import unittest


def _repo_root_from_runfiles() -> pathlib.Path:
    test_srcdir = os.environ.get("TEST_SRCDIR")
    test_workspace = os.environ.get("TEST_WORKSPACE")
    if not test_srcdir or not test_workspace:
        raise RuntimeError(
            "Expected TEST_SRCDIR and TEST_WORKSPACE to be set by Bazel test environment"
        )
    return pathlib.Path(test_srcdir) / test_workspace


class GenerateSymbolCheckReportTest(unittest.TestCase):
    def test_uses_bazel_compile_raw_invocation_and_flags(self):
        repo_root = _repo_root_from_runfiles()
        script_path = repo_root / "evergreen" / "generate_symbol_check_report.py"
        self.assertTrue(script_path.exists(), f"Missing runfile: {script_path}")

        # Simulate the Evergreen layout:
        # - script runs with cwd=.../src
        # - expansions.yml lives one directory above cwd (../expansions.yml)
        with tempfile.TemporaryDirectory() as td:
            workdir = pathlib.Path(td)
            (workdir / "expansions.yml").write_text('run_for_symbol_check: "true"\n')

            src = workdir / "src"
            src.mkdir(parents=True)

            bazel_build_flags = "--config=evg --config=symbol-checker --keep_going"
            bazel_build_invocation = "bazel build --config=evg --config=symbol-checker //src/..."
            (src / ".bazel_build_flags").write_text(bazel_build_flags + "\n")
            (src / ".bazel_build_invocation").write_text(bazel_build_invocation + "\n")

            checked_dir = src / "bazel-bin" / "buildscripts" / "bazel_testbuilds"
            checked_dir.mkdir(parents=True)
            checked_path = checked_dir / "unit_test_checked"
            checked_payload = {
                "status": "failed",
                "target": "//buildscripts/bazel_testbuilds:unit_test",
                "sym_file": "ignored.sym",
                "missing": ["some_symbol"],
            }
            checked_path.write_text(json.dumps(checked_payload))

            old_cwd = os.getcwd()
            try:
                os.chdir(src)
                with self.assertRaises(SystemExit) as ctx:
                    runpy.run_path(str(script_path), run_name="__main__")
            finally:
                os.chdir(old_cwd)

            # With a failing _checked file, the report script should exit non-zero.
            self.assertEqual(ctx.exception.code, 1)

            # The helper artifact should prefer the exact invocation written by bazel_compile.sh.
            self.assertEqual(
                (src / "bazel-invocation.txt").read_text().strip(), bazel_build_invocation
            )

            # The report content should prefer the exact flags written by bazel_compile.sh.
            report = json.loads((src / "report.json").read_text())
            log_raw = report["results"][0]["log_raw"]
            expected_repro_target = checked_payload["target"] + "_with_debug"
            self.assertIn(
                f"bazel build {bazel_build_flags} {expected_repro_target}",
                log_raw,
            )


if __name__ == "__main__":
    unittest.main()
