"""Exercise native Bazel wrappers without running a real build."""

import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PYTHON_OS = "windows" if os.name == "nt" else "macos" if platform.system() == "Darwin" else "linux"
PYTHON_ARCH = {"aarch64": "arm64", "AMD64": "x86_64"}.get(platform.machine(), platform.machine())
PYTHON_TOOLCHAIN = f"{PYTHON_OS}_{PYTHON_ARCH}"


class BazelWrapperFallbackTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "src"
        (self.root / "tools").mkdir(parents=True)
        hook_dir = self.root / "bazel/wrapper_hook"
        hook_dir.mkdir(parents=True)
        repository = Path(__file__).resolve().parents[2]
        self.wrapper = "bazel.bat" if os.name == "nt" else "bazel"
        shutil.copy(repository / "tools" / self.wrapper, self.root / "tools" / self.wrapper)
        shutil.copy(repository / "bazel/wrapper_hook/bazel_commands.commands", hook_dir)
        self.bazel_log = self.root / "bazel.log"
        # Python acts as the fake Bazel executable: all invocations use `build`,
        # so it executes this file and records the remaining Bazel arguments.
        (self.root / "build").write_text(
            "import pathlib, sys\n"
            "with pathlib.Path('bazel.log').open('a') as log:\n"
            "    log.write(' '.join(sys.argv[1:]) + '\\n')\n"
            "sys.exit(7 if any(arg.startswith('@py_') and arg.endswith('//:all') for arg in sys.argv) else 0)\n"
        )
        self.env = {
            **os.environ,
            "BAZEL_REAL": sys.executable,
            "BAZELISK_SKIP_WRAPPER": "1",
            "CI": "true",
            "task_id": "task",
            "distro_id": "regular",
            "MONGO_BAZEL_WRAPPER_FALLBACK": "1",
            "MONGO_WRAPPER_OUTPUT_ALL": "1",
        }

    def _run(self, *args: str) -> subprocess.CompletedProcess[str]:
        command = ["cmd.exe", "/d", "/c"] if os.name == "nt" else ["bash"]
        return subprocess.run(
            [*command, str(self.root / "tools" / self.wrapper), "build", *args],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )

    def _install_python(self, exit_code: int) -> None:
        cache = self.root.parent / "cache"
        output = cache / "execroot/src"
        output.mkdir(parents=True)
        distro = cache / f"external/_main~setup_mongo_python_toolchains~py_{PYTHON_TOOLCHAIN}/dist"
        distro.parent.mkdir(parents=True)
        if os.name == "nt":
            for link, target in (
                (self.root / "bazel-src", output),
                (distro, Path(sys.executable).parent),
            ):
                subprocess.run(
                    ["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)],
                    capture_output=True,
                    check=True,
                )
            # A venv interpreter also needs its configuration above the Scripts directory.
            venv_config = Path(sys.executable).parents[1] / "pyvenv.cfg"
            if venv_config.exists():
                shutil.copy(venv_config, distro.parent / "pyvenv.cfg")
            (self.root / "bazel/wrapper_hook/wrapper_hook.py").write_text(
                f"import sys\nsys.exit({exit_code})\n"
            )
        else:
            (self.root / "bazel-src").symlink_to(output, target_is_directory=True)
            python = distro / "bin/python3"
            python.parent.mkdir(parents=True)
            python.write_text(f"#!/usr/bin/env bash\nexit {exit_code}\n")
            python.chmod(0o755)

    def test_ci_python_bootstrap_failure_does_not_start_release_build(self) -> None:
        result = self._run("--config=public-release-local", "//:mongod")
        self.assertEqual(7, result.returncode, result.stdout + result.stderr)
        calls = self.bazel_log.read_text().splitlines()
        self.assertEqual(1, len(calls), calls)
        self.assertTrue(all("@py_" in call and "//:all" in call for call in calls), calls)

    def test_ci_python_bootstrap_failure_cannot_bypass_rc_default(self) -> None:
        (self.root / ".bazelrc").write_text("build --config=public-release\n")
        result = self._run("//:mongod")
        self.assertEqual(7, result.returncode, result.stdout + result.stderr)
        self.assertEqual(1, len(self.bazel_log.read_text().splitlines()))

    def test_ci_wrapper_failure_preserves_status_and_does_not_start_release_build(self) -> None:
        for exit_code in (1, 2, 7):
            with self.subTest(exit_code=exit_code):
                self._write_wrapper_failure(exit_code)
                result = self._run("--config=public-release", "//:mongod")
                self.assertEqual(exit_code, result.returncode, result.stdout + result.stderr)
                self.assertFalse(self.bazel_log.exists(), result.stdout + result.stderr)

    def _write_wrapper_failure(self, exit_code: int) -> None:
        if not (self.root / "bazel-src").exists():
            self._install_python(exit_code)
        elif os.name == "nt":
            (self.root / "bazel/wrapper_hook/wrapper_hook.py").write_text(
                f"import sys\nsys.exit({exit_code})\n"
            )
        else:
            python = (
                self.root.parent
                / f"cache/external/_main~setup_mongo_python_toolchains~py_{PYTHON_TOOLCHAIN}/dist/bin/python3"
            )
            python.write_text(f"#!/usr/bin/env bash\nexit {exit_code}\n")

    def test_linter_failure_preserves_status_and_does_not_fall_back(self) -> None:
        self._install_python(3)
        result = self._run("--config=public-release", "//:mongod")
        self.assertEqual(3, result.returncode, result.stdout + result.stderr)
        self.assertFalse(self.bazel_log.exists(), result.stdout + result.stderr)

    def test_evergreen_metadata_disables_fallback_without_ci_flag(self) -> None:
        self.env["CI"] = ""
        for key in ("task_id", "EVG_TASK_ID", "distro_id"):
            with self.subTest(key=key):
                for other in ("task_id", "EVG_TASK_ID", "distro_id"):
                    self.env.pop(other, None)
                self.env[key] = "regular"
                self.bazel_log.unlink(missing_ok=True)
                result = self._run("--config=public-release", "//:mongod")
                self.assertEqual(7, result.returncode, result.stdout + result.stderr)
                self.assertEqual(1, len(self.bazel_log.read_text().splitlines()))

    def test_release_policy_rejection_preserves_status_and_never_falls_back(self) -> None:
        self._install_python(4)
        for ci in ("true", ""):
            with self.subTest(ci=ci):
                self.env["CI"] = ci
                result = self._run("--config=public-release", "//:mongod")
                self.assertEqual(4, result.returncode, result.stdout + result.stderr)
                self.assertFalse(self.bazel_log.exists(), result.stdout + result.stderr)

    def test_explicit_local_fallback_after_bootstrap_failure_is_allowed(self) -> None:
        self.env["CI"] = ""
        self.env.pop("task_id")
        self.env.pop("distro_id")
        result = self._run("//:mongod")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        calls = self.bazel_log.read_text().splitlines()
        self.assertEqual(2, len(calls), calls)
        self.assertEqual("//:mongod", calls[-1])

    def test_explicit_local_fallback_after_wrapper_failure_is_allowed(self) -> None:
        self._install_python(1)
        self.env["CI"] = ""
        self.env.pop("task_id")
        self.env.pop("distro_id")
        result = self._run("//:mongod")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(["//:mongod"], self.bazel_log.read_text().splitlines())

    def test_local_bootstrap_failure_without_opt_in_preserves_status(self) -> None:
        self.env["CI"] = ""
        self.env.pop("task_id")
        self.env.pop("distro_id")
        self.env.pop("MONGO_BAZEL_WRAPPER_FALLBACK")
        result = self._run("//:mongod")
        self.assertEqual(7, result.returncode, result.stdout + result.stderr)
        self.assertEqual(1, len(self.bazel_log.read_text().splitlines()))


if __name__ == "__main__":
    unittest.main()
