from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

import yaml

SCRIPT = pathlib.Path(__file__).with_name("record_mongodb_server_version.sh")
REPO_ROOT = SCRIPT.parents[1]


class RecordMongoDBServerVersionTest(unittest.TestCase):
    def test_native_recording_task_uses_archived_version(self) -> None:
        config = yaml.safe_load(
            (REPO_ROOT / "etc/evergreen_yml_components/tasks/compile_tasks_shared.yml").read_text()
        )
        task = next(
            task for task in config["tasks"] if task["name"] == "record_mongodb_server_version"
        )
        for version in ("9.0.0", "9.0.0-patch-test-version-id"):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temp_dir:
                root = pathlib.Path(temp_dir)
                src = root / "src"
                evergreen_dir = src / "evergreen"
                evergreen_dir.mkdir(parents=True)
                shutil.copy2(SCRIPT, evergreen_dir)
                (evergreen_dir / "prelude.sh").write_text("", encoding="utf-8")
                (evergreen_dir / "materialize_ibm_test_install.sh").write_text(
                    '#!/bin/sh\necho "Version recording must not rebuild mongod" >&2\nexit 1\n',
                    encoding="utf-8",
                )
                stale_binary = src / "bazel-bin/install/bin/mongod"
                stale_binary.parent.mkdir(parents=True)
                stale_binary.write_text(
                    '#!/bin/sh\necho "branch-default-version"\n', encoding="utf-8"
                )
                stale_binary.chmod(0o755)
                archived_binary = root / "mongod"
                archived_binary.write_text(f'#!/bin/sh\necho "{version}"\n', encoding="utf-8")
                archived_binary.chmod(0o755)
                archive = root / "archive-dist-test.tgz"
                with tarfile.open(archive, "w:gz") as tar:
                    tar.add(archived_binary, arcname="dist-test/bin/mongod")

                # Emulate the download, then run the real extraction and recording commands.
                # Host setup functions are unrelated to the binary selected for this probe.
                for command in task["commands"]:
                    if command.get("func") == "fetch binaries":
                        shutil.copy2(archive, src / "mongo-binaries.tgz")
                    elif command.get("func") == "extract binaries":
                        variables = command["vars"]
                        result = subprocess.run(
                            [
                                sys.executable,
                                str(REPO_ROOT / "evergreen/functions/binaries_extract.py"),
                                "--tarball=mongo-binaries.tgz",
                                f"--extraction-command={variables['decompress']}",
                                f"--change-dir={variables['extraction_change_dir']}",
                            ],
                            cwd=src,
                            capture_output=True,
                            text=True,
                            check=False,
                        )
                        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                    elif command.get("command") == "subprocess.exec":
                        result = subprocess.run(
                            [command["params"]["binary"], *command["params"]["args"]],
                            cwd=root,
                            env={**os.environ, "is_patch": str("-patch-" in version).lower()},
                            capture_output=True,
                            text=True,
                            check=False,
                        )
                        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

                self.assertEqual(f"{version}\n", (src / "version_info_mongod.txt").read_text())

    def test_missing_archived_binary_does_not_use_stale_bazel_output(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            evergreen_dir = root / "src/evergreen"
            evergreen_dir.mkdir(parents=True)
            shutil.copy2(SCRIPT, evergreen_dir)
            (evergreen_dir / "prelude.sh").write_text("", encoding="utf-8")
            stale_binary = root / "src/bazel-bin/install-dist-test/bin/mongod"
            stale_binary.parent.mkdir(parents=True)
            stale_binary.write_text('#!/bin/sh\necho "branch-default-version"\n', encoding="utf-8")
            stale_binary.chmod(0o755)
            result = subprocess.run(
                [
                    "bash",
                    str(evergreen_dir / SCRIPT.name),
                    "./version-record/dist-test/bin/mongod",
                    "./version_info_mongod.txt",
                ],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertNotIn(
                "branch-default-version", (root / "src/version_info_mongod.txt").read_text()
            )

    def test_skips_cross_architecture_binary(self) -> None:
        # A foreign-architecture mongod cannot execute on the build host: exec
        # fails with "cannot execute binary file" (status 126). The script must
        # skip the record instead of failing the archive task.
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            evergreen_dir = root / "src" / "evergreen"
            evergreen_dir.mkdir(parents=True)
            script = evergreen_dir / SCRIPT.name
            shutil.copy2(SCRIPT, script)
            (evergreen_dir / "prelude.sh").write_text("", encoding="utf-8")
            binary = root / "src" / "mongod"
            binary.write_bytes(b"\x7fELF\x02\x02\x01\x00foreign-architecture-payload")
            binary.chmod(0o755)
            output = root / "src" / "version.txt"

            result = subprocess.run(
                ["bash", str(script), str(binary), str(output)],
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("was built for a different architecture", result.stderr)
        self.assertFalse(output.exists())

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
