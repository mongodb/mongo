"""Exercise the production Windows launcher executable, including cmd metacharacters."""

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(sys.platform == "win32", "requires native Windows")
class WindowsTestLauncherTest(unittest.TestCase):
    def _launcher(self):
        return Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"] / "bazel/wtl.exe"

    def _launch(self, executable: str, runfiles_dir: str = ""):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "arguments.py"
            script.write_text(
                "import json, os, sys; print(json.dumps([sys.argv[1:], "
                "os.environ.get('GOLDEN_TEST_CONFIG_PATH'), "
                "os.environ.get('GOLDEN_TEST_OUTPUT_ROOT_PATTERN')])); sys.exit(7)",
                encoding="utf-8",
            )
            arguments = [
                "",
                "with spaces",
                'embedded"quote',
                "trailing slash\\",
                "%PATH%",
                "%WTL_TEST_DEFINED%",
                "&|<>^!",
                "unicode-\u2603",
                "",
            ]
            result = subprocess.run(
                [str(self._launcher()), executable, str(script), *arguments],
                env={
                    **os.environ,
                    "RUNFILES_DIR": runfiles_dir,
                    "TEST_TIMEOUT": "60",
                    "WTL_TEST_DEFINED": "must not expand",
                    "GOLDEN_TEST_CONFIG_PATH": "unused",
                    "GOLDEN_TEST_OUTPUT_ROOT_PATTERN": "unused",
                },
                capture_output=True,
                text=True,
                check=False,
                timeout=60,
            )
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(json.loads(result.stdout.strip()), [arguments, None, None])
        self.assertIn("Process-under-test started with PID:", result.stderr)

    def test_preserves_arguments_and_exit_status(self):
        self._launch(sys.executable)

    def test_uses_original_executable_instead_of_runfiles_copy(self):
        runfiles_dir = sys.executable + ".runfiles"
        runfiles_copy = str(Path(runfiles_dir) / "_main/deep/package" / Path(sys.executable).name)
        self._launch(runfiles_copy, runfiles_dir)

    def test_timeout_writes_minidump_and_stops_test(self):
        with tempfile.TemporaryDirectory() as directory:
            binary_directory = Path(directory) / "bin"
            binary_directory.mkdir()
            output_directory = Path(directory) / "test outputs"
            binary = binary_directory / Path(sys.executable).name
            # Copy instead of linking: Bazel output and the test temporary directory can
            # live on different Windows drives. PYTHONHOME/PATH locate the original runtime.
            shutil.copy2(sys.executable, binary)
            stdout_path = Path(directory) / "stdout.txt"
            stderr_path = Path(directory) / "stderr.txt"
            stdout_file = stdout_path.open("w", encoding="utf-8")
            stderr_file = stderr_path.open("w", encoding="utf-8")
            process = subprocess.Popen(
                [str(self._launcher()), str(binary), "-c", "import time; time.sleep(60)"],
                env={
                    **os.environ,
                    "RUNFILES_DIR": "",
                    "TEST_TIMEOUT": "3",
                    "TEST_UNDECLARED_OUTPUTS_DIR": str(output_directory),
                    "PATHEXT": ".CPL",
                    "PYTHONHOME": sys.base_prefix,
                    # The copied executable still needs the runtime DLLs before Python can
                    # interpret PYTHONHOME. Keep the original runtime directories on PATH.
                    "PATH": os.pathsep.join(
                        [
                            str(Path(sys.executable).parent),
                            sys.base_prefix,
                            os.environ.get("PATH", ""),
                        ]
                    ),
                },
                stdout=stdout_file,
                stderr=stderr_file,
                text=True,
            )
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                # Killing only wtl.exe leaves PowerShell and the test holding the pipes open
                # and prevents Windows from deleting the copied executable during cleanup.
                subprocess.run(
                    [
                        str(Path(os.environ["SystemRoot"]) / "System32/taskkill.exe"),
                        "/T",
                        "/F",
                        "/PID",
                        str(process.pid),
                    ],
                    capture_output=True,
                    check=False,
                    timeout=30,
                )
                self.fail("Launcher did not stop the timed-out test")
            finally:
                stdout_file.close()
                stderr_file.close()
            stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
            result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertIn("Writing minidump to", result.stderr)
            dumps = list(output_directory.glob("*.mdmp"))
            self.assertEqual(len(dumps), 1, result.stderr)
            self.assertGreater(dumps[0].stat().st_size, 0)
            process_id = re.search(r"Process-under-test started with PID: (\d+)", result.stderr)
            self.assertIsNotNone(process_id, result.stderr)
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel32.OpenProcess.restype = ctypes.c_void_p
            kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
            # SYNCHRONIZE: an exited process signals its handle. OpenProcess may also fail
            # with ERROR_INVALID_PARAMETER when the terminated process no longer exists.
            handle = kernel32.OpenProcess(0x00100000, False, int(process_id.group(1)))
            if handle:
                try:
                    self.assertEqual(kernel32.WaitForSingleObject(handle, 5000), 0, result.stderr)
                finally:
                    kernel32.CloseHandle(handle)
            else:
                self.assertEqual(ctypes.get_last_error(), 87, result.stderr)
            self.assertEqual(dumps[0].name, f"dump_{binary.stem}.{process_id.group(1)}.mdmp")
            self.assertFalse(list(binary_directory.glob("*.mdmp")))


if __name__ == "__main__":
    # Evergreen's unit task passes C++ reporter args to every test.
    unittest.main(argv=[sys.argv[0]])
