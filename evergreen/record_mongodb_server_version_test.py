from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("record_mongodb_server_version.sh")


class RecordMongoDBServerVersionTest(unittest.TestCase):
    def test_uses_declared_install_output_when_convenience_link_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            evergreen_dir = root / "src" / "evergreen"
            evergreen_dir.mkdir(parents=True)
            script = evergreen_dir / SCRIPT.name
            shutil.copy2(SCRIPT, script)
            (evergreen_dir / "prelude.sh").write_text("", encoding="utf-8")
            binary = root / "src" / "bazel-bin" / "install-dist-test" / "bin" / "mongod"
            binary.parent.mkdir(parents=True)
            binary.write_text(
                "#!/bin/sh\n" 'printf "declared-output-version\\n"\n',
                encoding="utf-8",
            )
            binary.chmod(0o755)
            output = root / "src" / "version.txt"

            result = subprocess.run(
                [
                    "bash",
                    str(script),
                    "./bazel-bin/install/bin/mongod",
                    str(output),
                ],
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
            )
            output_text = output.read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "recording version from ./bazel-bin/install-dist-test/bin/mongod", result.stderr
        )
        self.assertEqual(output_text, "declared-output-version\n")

    def test_uses_bazel_out_install_output_when_bazel_bin_is_a_plain_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            evergreen_dir = root / "src" / "evergreen"
            evergreen_dir.mkdir(parents=True)
            script = evergreen_dir / SCRIPT.name
            shutil.copy2(SCRIPT, script)
            (evergreen_dir / "prelude.sh").write_text("", encoding="utf-8")
            binary = (
                root
                / "src"
                / "bazel-out"
                / "ppc-opt"
                / "bin"
                / "install-dist-test"
                / "bin"
                / "mongod"
            )
            binary.parent.mkdir(parents=True)
            binary.write_text(
                "#!/bin/sh\n" 'printf "bazel-out-version\\n"\n',
                encoding="utf-8",
            )
            binary.chmod(0o755)
            (root / "src" / "bazel-bin").mkdir()
            output = root / "src" / "version.txt"

            result = subprocess.run(
                [
                    "bash",
                    str(script),
                    "./bazel-bin/install/bin/mongod",
                    str(output),
                ],
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
            )
            output_text = output.read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "recording version from ./bazel-out/ppc-opt/bin/install-dist-test/bin/mongod",
            result.stderr,
        )
        self.assertEqual(output_text, "bazel-out-version\n")

    def test_failure_reports_binary_diagnostics_and_preserves_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            evergreen_dir = root / "src" / "evergreen"
            evergreen_dir.mkdir(parents=True)
            script = evergreen_dir / SCRIPT.name
            shutil.copy2(SCRIPT, script)
            (evergreen_dir / "prelude.sh").write_text("", encoding="utf-8")
            binary = root / "src" / "mongod"
            output = root / "src" / "version.txt"
            binary.write_text(
                "#!/bin/sh\n"
                'echo "startup rejected test binary"\n'
                'echo "loader rejected test binary" >&2\n'
                "exit 23\n",
                encoding="utf-8",
            )
            binary.chmod(0o755)

            result = subprocess.run(
                ["bash", str(script), str(binary), str(output)],
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
            )
            output_text = output.read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn("startup rejected test binary", result.stderr)
        self.assertIn("loader rejected test binary", result.stderr)
        self.assertIn("MongoDB server version command failed", result.stderr)
        self.assertIn(str(binary), result.stderr)
        self.assertEqual(output_text, "startup rejected test binary\n")


if __name__ == "__main__":
    unittest.main()
