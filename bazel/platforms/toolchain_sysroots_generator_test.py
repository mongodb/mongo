import hashlib
import io
import json
import os
import pathlib
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

from bazel.platforms import remote_execution_containers_generator_test as fixtures
from bazel.platforms import toolchain_sysroots_generator as generator


class ToolchainSysrootsTest(unittest.TestCase):
    def _run(
        self,
        publish: bool = False,
        *,
        fail: str = "",
        omitted: str = "",
        architectures: str = "ppc64le,s390x",
    ) -> tuple[int, list[list[str]], dict, dict]:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            output, manifest = root / "output", root / "pins.json"
            original = {
                "schema-version": 1,
                "sysroots": {
                    "rhel9": {
                        "arm64": {
                            "sha256": "e" * 64,
                            "url": "https://mdb-build-public.s3.amazonaws.com/toolchain-sysroots/toolchain-sysroot-rhel9-arm64-"
                            + "e" * 64
                            + ".tar.gz",
                        }
                    }
                },
            }
            manifest.write_text(json.dumps(original))
            commands = []

            def run(command: list[str], **kwargs):
                commands.append(command)
                if command[0] == "bash":
                    self.assertIn("--toolchain-sysroots", command)
                    directory = pathlib.Path(command[command.index("--output-dir") + 1]) / "rhel89"
                    directory.mkdir(parents=True)
                    (directory / "dockerfile").write_text(
                        "FROM redhat/ubi8:8.9@sha256:" + "a" * 64 + "\n"
                    )
                if command[:3] == ["docker", "buildx", "build"]:
                    architecture = command[command.index("--platform") + 1].split("/")[1]
                    if architecture == fail:
                        raise subprocess.CalledProcessError(1, command)
                if command[:2] == ["docker", "inspect"]:
                    return mock.Mock(stdout="sha256:loaded\n")
                if command[:2] == ["docker", "run"]:
                    return mock.Mock(stdout="glibc-2.28\nsystemtap-sdt-devel-4.9\n")
                if command[:2] == ["docker", "create"]:
                    return mock.Mock(
                        stdout="container-" + command[command.index("--platform") + 1].split("/")[1]
                    )
                if command[:2] == ["docker", "export"]:
                    fixtures.ToolchainSysrootArchiveTest._rootfs(
                        pathlib.Path(command[3]),
                        command[4].removeprefix("container-"),
                        omitted=omitted,
                    )
                if command[0] == "aws" and fail == "upload":
                    raise subprocess.CalledProcessError(1, command)
                return mock.Mock()

            argv = [
                "--publish" if publish else "--no-push",
                "--distro",
                "rhel8",
                "--output-dir",
                str(output),
                "--manifest",
                str(manifest),
            ]
            if not publish:
                argv += ["--architectures", architectures]
            with (
                mock.patch.object(generator.containers, "log_subprocess_run", side_effect=run),
                mock.patch.object(generator.containers, "create_buildx_builder"),
                mock.patch.dict(os.environ, {"is_patch": "false"}),
                mock.patch("sys.stdout", new_callable=io.StringIO),
            ):
                result = generator.main(argv)
            report = json.loads((output / "validation.json").read_text())
            pins = json.loads(manifest.read_text())
            for metadata in report["sysroots"].get("rhel8", {}).values():
                archive = output / metadata["archive"]
                self.assertEqual(
                    metadata["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest()
                )
                with tarfile.open(archive) as packaged:
                    provenance = json.load(packaged.extractfile("sysroot-manifest.json"))
                    self.assertEqual("cross-toolchain-runtime", provenance["purpose"])
                    self.assertIn("usr/include/sys/sdt.h", provenance["validated-files"])
                    self.assertEqual(
                        metadata["recipe-sha256"],
                        hashlib.sha256(provenance["recipe"].encode()).hexdigest(),
                    )
                    self.assertEqual("redhat/ubi8:8.9@sha256:" + "a" * 64, provenance["base-image"])
            self.assertFalse(
                any(
                    "--push" in command or command[:2] == ["docker", "login"]
                    for command in commands
                )
            )
            self.assertFalse(any(isinstance(command, str) for command in commands))
            if not publish or fail:
                self.assertEqual(original, pins)
            return result, commands, report, pins

    def test_no_push_builds_ibm_sysroots_without_publication(self) -> None:
        result, commands, report, _ = self._run()
        self.assertEqual(0, result)
        self.assertEqual({"ppc64le", "s390x"}, set(report["sysroots"]["rhel8"]))
        self.assertFalse(any(command[0] == "aws" for command in commands))

    def test_publication_only_uploads_hash_addressed_sysroots_and_preserves_other_pins(
        self,
    ) -> None:
        result, commands, report, pins = self._run(True)
        self.assertEqual(0, result)
        self.assertEqual({"amd64", "arm64", "ppc64le", "s390x"}, set(pins["sysroots"]["rhel8"]))
        self.assertEqual("e" * 64, pins["sysroots"]["rhel9"]["arm64"]["sha256"])
        uploads = [command for command in commands if command[0] == "aws"]
        self.assertEqual(4, len(uploads))
        for command in uploads:
            self.assertIn("s3://mdb-build-public/toolchain-sysroots/", command[4])
            self.assertTrue(
                any(
                    metadata["sha256"] in command[4]
                    for metadata in report["sysroots"]["rhel8"].values()
                )
            )

    def test_upload_failure_preserves_manifest(self) -> None:
        result, _, report, _ = self._run(True, fail="upload")
        self.assertEqual(1, result)
        self.assertEqual(["rhel8"], report["failed_distros"])

    def test_invalid_publication_pins_are_rejected_before_any_upload(self) -> None:
        metadata = {
            architecture: {
                "sha256": "a" * 64,
                "archive": f"toolchain-sysroot-rhel8-{architecture}-{'a' * 64}.tar.gz",
            }
            for architecture in ("s390x", "ppc64le")
        }
        metadata["ppc64le"]["archive"] = metadata["ppc64le"]["archive"].replace("rhel8", "rhel9")
        with (
            mock.patch.object(generator.containers, "log_subprocess_run") as run,
            mock.patch.object(generator.containers, "sysroot_upload_environment") as credentials,
        ):
            with self.assertRaisesRegex(ValueError, "sysroots.rhel8.ppc64le.url"):
                generator.publish_sysroots(
                    "rhel8",
                    pathlib.Path("output"),
                    metadata,
                    "mdb-build-public",
                    "toolchain-sysroots",
                )
            run.assert_not_called()
            credentials.assert_not_called()

    def test_missing_sdt_header_prevents_upload(self) -> None:
        result, commands, _, _ = self._run(True, omitted="usr/include/sys/sdt.h")
        self.assertEqual(1, result)
        self.assertFalse(any(command[0] == "aws" for command in commands))
        self.assertTrue(any(command[:2] == ["docker", "rm"] for command in commands))

    def test_failed_architecture_fails_no_push(self) -> None:
        result, _, report, _ = self._run(fail="s390x")
        self.assertEqual(1, result)
        self.assertEqual(["rhel8"], report["failed_distros"])

    def test_patch_cannot_publish(self) -> None:
        with (
            mock.patch.dict(os.environ, {"is_patch": "true"}),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            mock.patch.object(generator.containers, "log_subprocess_run") as run,
        ):
            with self.assertRaises(SystemExit):
                generator.main(["--publish"])
            run.assert_not_called()

    def test_invalid_architecture_fails_before_build(self) -> None:
        with (
            mock.patch("sys.stderr", new_callable=io.StringIO),
            mock.patch.object(generator.containers, "log_subprocess_run") as run,
        ):
            with self.assertRaises(SystemExit):
                generator.main(["--no-push", "--distro", "rhel8", "--architectures", "wrong"])
            run.assert_not_called()

    def test_malformed_manifest_fails_before_build_or_upload(self) -> None:
        for sysroots in (
            {"rhel8": []},
            {"rhel8": {"s390x": []}},
            {"rhel8": {"s390x": {}}},
            {"rhel8": {"s390x": {"sha256": 7}}},
        ):
            with self.subTest(sysroots=sysroots), tempfile.TemporaryDirectory() as tmp:
                manifest = pathlib.Path(tmp) / "pins.json"
                content = json.dumps({"schema-version": 1, "sysroots": sysroots})
                manifest.write_text(content)
                with (
                    mock.patch.dict(os.environ, {"is_patch": "false"}),
                    mock.patch("sys.stderr", new_callable=io.StringIO) as stderr,
                    mock.patch.object(generator.containers, "log_subprocess_run") as run,
                    mock.patch.object(generator, "build_sysroots") as build,
                    mock.patch.object(generator, "publish_sysroots") as upload,
                ):
                    with self.assertRaises(SystemExit) as error:
                        generator.main(
                            [
                                "--publish",
                                "--manifest",
                                str(manifest),
                                "--output-dir",
                                str(pathlib.Path(tmp) / "output"),
                                "--install-binfmt",
                            ]
                        )
                    self.assertEqual(2, error.exception.code)
                    self.assertIn(str(manifest), stderr.getvalue())
                    self.assertIn("sysroots.rhel8", stderr.getvalue())
                    run.assert_not_called()
                    build.assert_not_called()
                    upload.assert_not_called()
                self.assertEqual(content, manifest.read_text())


class EvergreenToolchainWorkflowTest(unittest.TestCase):
    def _run_generate(
        self,
        is_patch: str,
        *,
        no_push: bool = False,
        distro: str = "rhel9",
        startup_failures: int = 0,
        generator_exit: int = 0,
        missing_optional_os: bool = False,
    ) -> tuple[subprocess.CompletedProcess[str], list[str], str]:
        source = pathlib.Path(__file__).parents[2] / "evergreen/toolchain_sysroots_generate.sh"
        with tempfile.TemporaryDirectory() as temp_dir:
            workspace = pathlib.Path(temp_dir)
            evergreen = workspace / "src/evergreen"
            evergreen.mkdir(parents=True)
            script = evergreen / source.name
            script.write_text(source.read_text())
            # External operations are replaced; even a regression cannot touch host Docker.
            (evergreen / "prelude.sh").write_text(
                """\
sudo() {
    printf 'sudo %s\\n' "$*" >> "$OPERATIONS"
    if [[ $1 == jq && $2 != -e ]]; then
        printf '%s\\n' '{"data-root":"/data/mci/docker","log-driver":"journald"}'
    elif [[ $1 == tee ]]; then
        cat >> "$OPERATIONS"
    fi
}
docker() {
    printf 'docker %s\\n' "$*" >> "$OPERATIONS"
    if [[ $1 == info && $STARTUP_FAILURES -gt 0 ]]; then
        STARTUP_FAILURES=$((STARTUP_FAILURES - 1))
        return 1
    fi
}
sleep() { :; }
aws() { printf 'aws %s\\n' "$*" >> "$OPERATIONS"; }
git() { :; }
bazel() { printf 'bazel %s\\n' "$*" >> "$OPERATIONS"; }
activate_venv() {
    if [[ $MISSING_OPTIONAL_OS == true ]]; then
        test "$OS" != Windows_NT
    fi
}
capture_python() {
    if [[ " $* " == *" --no-push "* ]]; then
        test -z "${AWS_ACCESS_KEY_ID:-}${AWS_SECRET_ACCESS_KEY:-}${aws_key_build:-}${aws_secret_build:-}" || exit 90
    fi
    printf '%s\\n' "$@" > "$CAPTURE_ARGS"
    return "$GENERATOR_EXIT"
}
python=capture_python
"""
            )
            repin = workspace / "src/bazel/remote_execution_container/repin_dockerfiles.sh"
            repin.parent.mkdir(parents=True)
            repin.touch()
            captured = workspace / "generator-args"
            operations = workspace / "operations"
            environment = {
                "PATH": os.environ["PATH"],
                "CAPTURE_ARGS": str(captured),
                "OPERATIONS": str(operations),
                "STARTUP_FAILURES": str(startup_failures),
                "GENERATOR_EXIT": str(generator_exit),
                "MISSING_OPTIONAL_OS": str(missing_optional_os).lower(),
                "toolchain_sysroot_distro": distro,
                "is_patch": is_patch,
                "AWS_ACCESS_KEY_ID": "unused-fixture-key",
                "AWS_SECRET_ACCESS_KEY": "unused-fixture-key",
                "aws_key_build": "unused-fixture-key",
                "aws_secret_build": "unused-fixture-key",
            }
            result = subprocess.run(
                ["bash", "-x", str(script), *(["--no-push"] if no_push else ["--publish"])],
                cwd=workspace,
                env=environment,
                capture_output=True,
                text=True,
            )
            status = workspace / "src/toolchain-sysroot-output/exit-status"
            if status.exists():
                self.assertEqual(str(result.returncode), status.read_text().strip())
            return (
                result,
                captured.read_text().splitlines() if captured.exists() else [],
                operations.read_text() if operations.exists() else "",
            )

    def test_production_runs_publish_sysroots(self) -> None:
        result, args, operations = self._run_generate("false")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("--publish", args)
        self.assertIn("--distro=rhel9", args)
        self.assertNotIn("--no-push", args)
        self.assertNotIn("aws ", operations)
        self.assertNotIn("bazel ", operations)

    def test_docker_config_is_written_without_shared_temporary_files(self) -> None:
        result, _, operations = self._run_generate("true")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("sudo tee /etc/docker/daemon.json", operations)
        self.assertIn('{"data-root":"/data/mci/docker","log-driver":"journald"}', operations)
        self.assertNotIn("/tmp/toolchain-docker-daemon.json", operations)
        self.assertNotIn("sudo cp ", operations)

    def test_failed_validation_records_and_returns_its_failure(self) -> None:
        result, args, _ = self._run_generate("true", generator_exit=1)
        self.assertEqual(1, result.returncode)
        self.assertIn("--no-push", args)

    def test_existing_venv_helper_can_read_unset_optional_os(self) -> None:
        result, args, _ = self._run_generate("true", missing_optional_os=True)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("--no-push", args)

    def test_patch_and_explicit_no_push_runs_skip_all_publication(self) -> None:
        for is_patch, no_push in (("true", False), ("true", True), ("false", True)):
            with self.subTest(is_patch=is_patch, no_push=no_push):
                result, args, operations = self._run_generate(is_patch, no_push=no_push)
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertIn("--no-push", args)
                self.assertIn("--output-dir=toolchain-sysroot-output", args)
                self.assertFalse(any(arg.startswith("--sysroot-s3") for arg in args))
                self.assertNotIn("aws ", operations)
                self.assertNotIn("docker login", operations)
                self.assertNotIn("bazel ", operations)

    def test_waits_for_docker_startup_before_repinning(self) -> None:
        result, args, operations = self._run_generate("true", startup_failures=2)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(3, operations.splitlines().count("docker info"))
        self.assertIn("--no-push", args)

    def test_docker_startup_failure_is_bounded_and_skips_generation(self) -> None:
        result, args, operations = self._run_generate("true", startup_failures=100)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("Docker did not become ready", result.stderr)
        self.assertEqual(30, operations.splitlines().count("docker info"))
        self.assertEqual([], args)


class ToolchainRecipeTest(unittest.TestCase):
    def test_toolchain_profile_requires_a_separate_output_directory(self) -> None:
        source = (
            pathlib.Path(__file__).parents[1] / "remote_execution_container/repin_dockerfiles.sh"
        )
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                ["bash", str(source), "--toolchain-sysroots"],
                env={**os.environ, "BUILD_WORKSPACE_DIRECTORY": tmp},
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("requires --output-dir", result.stderr)
            self.assertEqual([], list(pathlib.Path(tmp).iterdir()))

    def _repin(
        self, distro: str, pull_failures: int = 0
    ) -> tuple[subprocess.CompletedProcess[str], list[str], str]:
        source = (
            pathlib.Path(__file__).parents[1] / "remote_execution_container/repin_dockerfiles.sh"
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            script = root / "bazel/remote_execution_container/repin_dockerfiles.sh"
            script.parent.mkdir(parents=True)
            script.write_text(source.read_text())
            bindir = root / "bin"
            bindir.mkdir()
            (bindir / "docker").write_text("""#!/bin/bash
printf '%s\\n' "$*" >> "$DOCKER_LOG"
case $1 in
pull)
    attempts=$(cat "$PULL_COUNT" 2>/dev/null || echo 0)
    attempts=$((attempts + 1))
    printf '%s' "$attempts" > "$PULL_COUNT"
    if ((attempts <= PULL_FAILURES)); then exit 1; fi
    ;;
inspect) printf 'repo@sha256:%064d\\n' 0 ;;
run) printf '%s\\n' glibc-devel-2.28-251.el8_10.40 libxcrypt-devel-4.1.1-6.el8 ;;
esac
""")
            (bindir / "sleep").write_text("#!/bin/bash\nexit 0\n")
            for path in bindir.iterdir():
                path.chmod(0o755)
            result = subprocess.run(
                [
                    "bash",
                    str(script),
                    "--toolchain-sysroots",
                    "--output-dir",
                    str(script.parent / "output"),
                ],
                env={
                    **os.environ,
                    "BUILD_WORKSPACE_DIRECTORY": str(root),
                    "PATH": f"{bindir}:{os.environ['PATH']}",
                    "toolchain_sysroot_distro": distro,
                    "DOCKER_LOG": str(root / "docker.log"),
                    "PULL_COUNT": str(root / "pull.count"),
                    "PULL_FAILURES": str(pull_failures),
                },
                capture_output=True,
                text=True,
            )
            recipes = [
                path.relative_to(script.parent / "output").as_posix()
                for path in (script.parent / "output").glob("*/dockerfile")
            ]
            log = (root / "docker.log").read_text() if (root / "docker.log").exists() else ""
            return result, recipes, log

    def test_selected_rhel_recipe_requests_crypt_development_headers(self) -> None:
        for distro, recipe in (("rhel8", "rhel89"), ("rhel9", "rhel93"), ("rhel10", "rhel10")):
            with self.subTest(distro=distro):
                result, recipes, log = self._repin(distro)
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertEqual([f"{recipe}/dockerfile"], recipes)
                self.assertIn("libxcrypt-devel", log)
                self.assertIn("systemtap-sdt-devel", log)
                self.assertIn("kernel-headers", log)
                self.assertNotIn("cyrus-sasl-devel", log)
                self.assertNotIn("openssl-devel", log)

    def test_pull_failure_is_retried_without_using_stale_pins(self) -> None:
        for failures, expected_attempts in ((1, 2), (100, 3)):
            with self.subTest(failures=failures):
                result, recipes, log = self._repin("rhel8", failures)
                self.assertEqual(
                    expected_attempts, sum(line.startswith("pull ") for line in log.splitlines())
                )
                self.assertEqual(failures < 3, result.returncode == 0)
                if failures >= 3:
                    self.assertEqual([], recipes)
                    self.assertNotIn("inspect ", log)

    def test_unknown_distro_fails_before_container_operations(self) -> None:
        result, recipes, log = self._repin("rhle8")
        self.assertNotEqual(0, result.returncode)
        self.assertEqual([], recipes)
        self.assertEqual("", log)


if __name__ == "__main__":
    unittest.main()
