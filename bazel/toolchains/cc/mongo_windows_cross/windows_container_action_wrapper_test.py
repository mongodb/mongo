"""Unit tests for the Windows persistent-container action runner."""

from __future__ import annotations

import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from bazel.toolchains.cc.mongo_windows_cross import windows_container_action_wrapper as wrapper

shared = wrapper.shared


class FakeDocker:
    """Simulates the Docker CLI against the TCP-reached WSL engine.

    ``records`` maps the name of a shell script started through the detached exec channel
    (``windows-action-shim``, ``mount-probe``, ``diagnostics``) to the output and exit status
    the container should produce, or to a callable taking the container command and returning
    them. Output only becomes visible through ``docker cp`` once the command "ran".
    """

    def __init__(self, records=None, running=(), existing=()):
        self.records = dict(records or {})
        self.running = set(running)
        self.existing = set(existing) | set(running)
        self.files: dict[str, str] = {}
        self.calls: list[list[str]] = []
        self.environments: list[str] = []
        self.env_file_unsupported = False

    def __call__(self, args, *, stdout=None, stderr=None, timeout=None):
        args = list(args)
        self.calls.append(args)

        def done(returncode=0, out="", err=""):
            return subprocess.CompletedProcess(args, returncode, stdout=out, stderr=err)

        command = args[1] if len(args) > 1 else ""
        if command == "info":
            return done(out="linux\n")
        if command == "inspect":
            name = args[-1]
            if "-f" in args:
                template = args[args.index("-f") + 1]
                if name.startswith("mongo_cross_helper_"):
                    return done(out="exited 0\n")
                if name not in self.existing:
                    return done(returncode=1, err=f"Error: No such object: {name}\n")
                if template == "{{.State.Running}}":
                    return done(out="true\n" if name in self.running else "false\n")
                if template == "{{.Id}}":
                    return done(out=f"id-{name}\n")
                return done(out=f"running image=image {name}\n")
            return done() if name in self.existing else done(returncode=1)
        if command == "ps":
            return done(out="")
        if command == "run":
            name = args[args.index("--name") + 1]
            if "--privileged" in args:
                return done(out="helper-id\n")
            self.existing.add(name)
            self.running.add(name)
            return done(out="container-id\n")
        if command == "start":
            self.running.add(args[-1])
            return done()
        if command == "rm":
            self.existing.discard(args[-1])
            self.running.discard(args[-1])
            return done()
        if command == "logs":
            return done(out="/mnt/z drvfs\n")
        if command == "exec":
            if "--env-file" in args:
                self.environments.append(
                    pathlib.Path(args[args.index("--env-file") + 1]).read_text(encoding="utf-8")
                )
            if self.env_file_unsupported and "--env-file" in args:
                return done(returncode=125, err="unknown flag: --env-file\n")
            capture = shared.ACTION_OUTPUT_CAPTURE_SCRIPT
            if capture not in args:
                return done()
            container_command = args[args.index(capture) + 1 :]
            log_env = next(
                token for token in args if token.startswith(shared.ACTION_OUTPUT_LOG_ENV + "=")
            )
            log_path = log_env.split("=", 1)[1]
            key = (
                container_command[3]
                if container_command[:2] == ["/bin/sh", "-c"]
                else container_command[0]
            )
            record = self.records.get(key, ("", 0))
            output, status = record(container_command) if callable(record) else record
            self.files[log_path] = output
            self.files[f"{log_path}.exit"] = f"{status}\n"
            return done()
        if command == "cp":
            if not args[2].startswith(tuple(f"{name}:" for name in self.existing)):
                destination = args[3].split(":", 1)[1]
                self.files[destination] = pathlib.Path(args[2]).read_text(encoding="utf-8")
                if destination.endswith(shared.ACTION_CANCEL_SUFFIX):
                    # A rejected pre-exec launch has no process to stop. A cancellation
                    # request acknowledges that state, just as a supervisor does after
                    # stopping a launched action.
                    log_path = destination.removesuffix(shared.ACTION_CANCEL_SUFFIX)
                    self.files[log_path + shared.ACTION_EXIT_FILE_SUFFIX] = "125\n"
                return done()
            source = args[2].split(":", 1)[1]
            destination = pathlib.Path(args[3])
            if source not in self.files:
                return done(returncode=1, err="Could not find the file\n")
            (destination / pathlib.PurePosixPath(source).name).write_text(
                self.files[source], encoding="utf-8"
            )
            return done()
        return done()

    def commands(self, name: str) -> list[list[str]]:
        return [call for call in self.calls if len(call) > 1 and call[1] == name]


def _fake_container_path(path, prefix="/mnt"):
    """Maps a POSIX test path below ``/`` to ``/mnt/z/...`` so real files can stand in for Z:."""
    return pathlib.PurePosixPath(prefix, "z", *pathlib.PurePosixPath(os.fspath(path)).parts[1:])


class WindowsContainerActionWrapperTest(unittest.TestCase):
    def setUp(self) -> None:
        self.environ = mock.patch.dict(os.environ, {}, clear=True)
        self.environ.start()
        self.addCleanup(self.environ.stop)

    @staticmethod
    def _config(**overrides):
        config = {
            "image": "quay.io/mongodb/rbe@sha256:abc123",
            "docker_command": "C:/Program Files/Docker/docker.exe",
            "network": "host",
            "repo_root": "Z:\\data\\mci\\src",
            "sandbox_base": "Z:\\b\\b-mongo-action-sandbox",
            "shared_install_dir": "Z:\\b\\b-mongo-shared-install",
            "container_prefix": "mongo_windows_cross_action_amazon_linux_2023_x86_64_abc",
            "container_layout_version": "v1",
            "docker_host_mode": "wsl",
            "drive_mount_prefix": "/mnt",
            "mount_paths": ["C:\\llvm", "Z:\\data\\mci\\src\\.tmp\\sysroot"],
            "runner_environment": {"SYSTEMROOT": "C:\\Windows", "USERPROFILE": "C:\\Users\\agent"},
        }
        config.update(overrides)
        return config

    def test_container_path_translates_drive_paths_below_the_wsl_mount_prefix(self):
        self.assertEqual(
            wrapper._container_path("Z:\\b\\b\\execroot\\_main").as_posix(),
            "/mnt/z/b/b/execroot/_main",
        )
        self.assertEqual(
            wrapper._container_path("c:/llvm/bin/lld-link", "/drives").as_posix(),
            "/drives/c/llvm/bin/lld-link",
        )
        self.assertEqual(
            wrapper._container_path(pathlib.PureWindowsPath("Z:\\")).as_posix(), "/mnt/z"
        )
        with self.assertRaises(ValueError):
            wrapper._container_path("external/tools/lld-link")
        with self.assertRaises(ValueError):
            wrapper._container_path("/mnt/z/already/posix")

    def test_container_mounts_expose_whole_drives_read_only_and_sandbox_writable(self):
        mounts = wrapper._container_mounts(self._config(), pathlib.PureWindowsPath("Z:\\b\\b"))

        self.assertEqual(
            mounts,
            [
                "-v",
                "/mnt/c:/mnt/c:ro",
                "-v",
                "/mnt/z:/mnt/z:ro",
                "-v",
                "/mnt/z/b/b-mongo-action-sandbox:/mnt/z/b/b-mongo-action-sandbox",
                "-v",
                "/mnt/z/b/b-mongo-shared-install:/mnt/z/b/b-mongo-shared-install",
            ],
        )

    def test_source_entrypoints_import_shared_module_in_runfiles_layout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = pathlib.Path(wrapper.__file__).resolve().parents[4]
            for relative in (
                "bazel/hermetic_container/container_paths.py",
                "bazel/toolchains/cc/container_action.py",
                "bazel/toolchains/cc/container_engine.py",
                "bazel/toolchains/cc/scripts/action_supervisor.sh",
                "bazel/toolchains/cc/scripts/input_probe_setup.sh",
                "bazel/toolchains/cc/scripts/input_probe.sh",
                "bazel/toolchains/cc/scripts/mount_windows_drives.sh",
                "bazel/toolchains/cc/mongo_windows_cross/windows_container_action_wrapper.py",
                "bazel/toolchains/cc/mongo_windows_cross/scripts/action.sh",
                "bazel/toolchains/cc/mongo_windows_cross/scripts/mount_probe.sh",
                "bazel/toolchains/cc/mongo_windows_cross/scripts/diagnostics.sh",
            ):
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(repo_root / relative, destination)
            result = subprocess.run(
                [
                    sys.executable,
                    str(
                        root
                        / "bazel/toolchains/cc/mongo_windows_cross/windows_container_action_wrapper.py"
                    ),
                ],
                cwd=root,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("usage:", result.stderr)
            self.assertNotIn("ImportError", result.stderr)

    def test_container_tool_and_tool_command(self):
        self.assertEqual(
            wrapper._container_tool("external\\toolchain\\wrappers\\lld-link.cmd", "/mnt"),
            "external/toolchain/wrappers/lld-link.cmd",
        )
        self.assertEqual(
            wrapper._container_tool("C:\\python\\python.exe", "/mnt"), "/mnt/c/python/python.exe"
        )
        self.assertEqual(
            wrapper._tool_command("external/toolchain/wrappers/lld-link.cmd"),
            ["/bin/sh", "external/toolchain/wrappers/lld-link.cmd"],
        )
        self.assertEqual(
            wrapper._tool_command("external/toolchain/tools/lld-link"),
            ["external/toolchain/tools/lld-link"],
        )

    def test_action_environment_keeps_the_bazel_action_environment_only(self):
        os.environ.update(
            {
                "PATH": "external/toolchain/wrappers;external/toolchain/tools",
                "LIB": "external/toolchain/lib/clang/windows;external/toolchain/msvc/lib/x64",
                "MSYS": "winsymlinks:nativestrict",
                "SYSTEMROOT": "C:\\Windows",
                "SystemDrive": "C:",
                "DOCKER_HOST": "tcp://127.0.0.1:2375",
                "TMP": "Z:\\b",
                "TEMP": "Z:\\b",
                "PWD": "/proc/self/cwd",
                shared.ACTION_OUTPUT_LOG_ENV: "/tmp/mongo-cross-actions/old.log",
            }
        )

        environment = wrapper._action_environment(
            pathlib.PurePosixPath("/mnt/z/b/b-mongo-action-sandbox/7/execroot/_main")
        )

        self.assertEqual(
            environment,
            {
                "PATH": "external/toolchain/wrappers;external/toolchain/tools",
                "LIB": "external/toolchain/lib/clang/windows;external/toolchain/msvc/lib/x64",
                "MSYS": "winsymlinks:nativestrict",
                "PWD": "/mnt/z/b/b-mongo-action-sandbox/7/execroot/_main",
                "MONGO_LINUX_ACTION_CONTAINER": "1",
                shared.ACTION_PRIVATE_SCRATCH_ENV: "1",
            },
        )

    def test_write_env_file_lists_sorted_assignments(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env_file = wrapper._write_env_file(
                {"LIB": "a;b", "PATH": "x;y", "INCLUDE": "c"}, pathlib.Path(temp_dir)
            )
            self.assertEqual(env_file.read_text(encoding="utf-8"), "INCLUDE=c\nLIB=a;b\nPATH=x;y\n")

    def test_apply_runner_environment_restores_docker_routing(self):
        wrapper._apply_runner_environment(self._config())

        self.assertEqual(os.environ["SYSTEMROOT"], "C:\\Windows")
        self.assertEqual(os.environ["USERPROFILE"], "C:\\Users\\agent")
        self.assertEqual(os.environ["DOCKER_HOST"], "tcp://127.0.0.1:2375")
        self.assertNotIn("DOCKER_CONTEXT", os.environ)

    def test_container_name_is_deterministic_and_honours_configured_name(self):
        output_base = pathlib.PureWindowsPath("Z:\\b\\b")
        first = wrapper._container_name(self._config(), output_base)
        second = wrapper._container_name(self._config(), output_base)
        other_image = wrapper._container_name(self._config(image="other@sha256:def"), output_base)
        named = wrapper._container_name(
            self._config(
                container_name="mongo_windows_cross_action_amazon_linux_2023_x86_64_abc_0123456789ab"
            ),
            output_base,
        )

        self.assertEqual(first, second)
        self.assertTrue(
            first.startswith("mongo_windows_cross_action_amazon_linux_2023_x86_64_abc_")
        )
        self.assertNotEqual(first, other_image)
        self.assertTrue(
            named.startswith(
                "mongo_windows_cross_action_amazon_linux_2023_x86_64_abc_0123456789ab_"
            )
        )

    def test_read_config_rejects_incomplete_configs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = pathlib.Path(temp_dir) / "config.json"
            config_path.write_text(json.dumps({"image": "img"}), encoding="utf-8")
            with self.assertRaises(ValueError):
                wrapper._read_config(config_path)
            self.assertEqual(wrapper.main(["runner", str(config_path), "tool"]), 2)
            self.assertEqual(wrapper.main(["runner"]), 2)

    def _write_config(self, root: pathlib.Path, **overrides) -> tuple[pathlib.Path, dict]:
        output_base = root / "output-base"
        sandbox_base = root / "sandbox"
        output_base.mkdir()
        sandbox_base.mkdir()
        values = {
            # Any existing executable: the fake intercepts every Docker CLI invocation.
            "docker_command": "/bin/sh",
            "sandbox_base": str(sandbox_base),
            "shared_install_dir": str(root / "shared-install"),
            "repo_root": str(root / "repo"),
            "mount_paths": [],
        }
        values.update(overrides)
        config = self._config(**values)
        (root / "repo").mkdir()
        config_path = output_base / "mongo_linux_container_actions.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        return config_path, config

    def test_run_in_container_runs_the_tool_detached_in_the_sandbox_execroot(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            config_path, config = self._write_config(root)
            docker = FakeDocker(
                records={"windows-action-shim": ("lld-link: error: undefined symbol: foo\n", 1)},
                running={wrapper._container_name(config, config_path.parent)},
            )
            os.environ.update(
                {"PATH": "external/toolchain/wrappers;external/toolchain/tools", "LIB": "a;b"}
            )
            sandbox_execroot = pathlib.PureWindowsPath(
                "Z:\\b\\b-mongo-action-sandbox\\bazel-sandbox.1\\persistent-container\\7\\execroot\\_main"
            )

            with (
                mock.patch.object(shared, "run", side_effect=docker),
                mock.patch.object(pathlib.Path, "cwd", return_value=sandbox_execroot),
                mock.patch("sys.stderr") as stderr,
            ):
                exit_code = wrapper._run_in_container(
                    config,
                    config_path,
                    "external\\toolchain\\wrappers\\lld-link.cmd",
                    ["@bazel-out/x64_windows-opt/bin/mongod.exe-2.params"],
                )

            self.assertEqual(exit_code, 1)
            written = "".join(call.args[0] for call in stderr.write.call_args_list)
            self.assertIn("lld-link: error: undefined symbol: foo", written)
            self.assertNotIn("diagnostics", written)

            launch = next(call for call in docker.commands("exec") if "-d" in call)
            self.assertEqual(launch[0], config["docker_command"])
            self.assertEqual(
                launch[1:5],
                [
                    "exec",
                    "-d",
                    "-w",
                    "/mnt/z/b/b-mongo-action-sandbox/bazel-sandbox.1/persistent-container/7/execroot/_main",
                ],
            )
            self.assertEqual(launch[5], "--env-file")
            self.assertEqual(
                launch[launch.index("windows-action-shim") :],
                [
                    "windows-action-shim",
                    "/bin/sh",
                    "external/toolchain/wrappers/lld-link.cmd",
                    "@bazel-out/x64_windows-opt/bin/mongod.exe-2.params",
                ],
            )
            self.assertIn(wrapper._ACTION_SHIM, launch)
            self.assertIn(f"{shared.ACTION_PRIVATE_SCRATCH_ENV}=1\n", docker.environments[0])
            # The environment file is removed once the action has finished.
            env_files = list(
                (root / "sandbox" / wrapper._RUNNER_SCRATCH_DIRNAME).glob("action-env-*")
            )
            self.assertEqual(env_files, [])
            self.assertFalse(docker.commands("run"))

    @unittest.skipIf(os.name == "nt", "Process-backed supervision needs POSIX shells")
    def test_supervisor_cleans_action_scratch_after_completion_cancellation_and_lease_expiry(self):
        for outcome in ("success", "failure", "cancel", "lease-expiry", "supervisor-term"):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as temp_dir:
                root = pathlib.Path(temp_dir)
                log = root / "action.log"
                ready = root / "ready"
                finish = root / "finish"
                command = [
                    "/bin/sh",
                    "-c",
                    wrapper._ACTION_SHIM,
                    "windows-action-shim",
                    "/bin/sh",
                    "-c",
                    # Ignore TERM to require the supervisor's KILL escalation. Publish
                    # the scratch path after writing data, then wait for the test.
                    'trap "" TERM; printf scratch >"$TMPDIR/link-scratch"; '
                    'printf "%s\\n%s\\n" "$TMPDIR" "$HOME" >"$1"; '
                    'while [ ! -f "$2" ]; do /bin/sleep 0.05; done; exit "$3"',
                    "linker",
                    str(ready),
                    str(finish),
                    "7" if outcome == "failure" else "0",
                ]
                _, capture = shared._action_output_capture(pathlib.PurePosixPath(log), command)
                if outcome == "lease-expiry":
                    capture[2] = capture[2].replace(
                        f"lease_seconds={shared.ACTION_LEASE_SECONDS}",
                        "lease_seconds=1",
                    )
                environment = dict(
                    os.environ,
                    MONGO_CROSS_ACTION_OUTPUT_LOG=str(log),
                    MONGO_CROSS_ACTION_PRIVATE_SCRATCH="1",
                    PATH="external/toolchain/tools;C:\\Windows",
                )
                process = subprocess.Popen(
                    capture,
                    env=environment,
                    start_new_session=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                action_tmp = None
                try:
                    deadline = time.monotonic() + 5
                    while not ready.exists():
                        self.assertIsNone(process.poll(), "action exited before writing scratch")
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.01)
                    action_tmp, home = map(pathlib.Path, ready.read_text().splitlines())
                    self.assertEqual(home, action_tmp / "home")
                    self.assertEqual((action_tmp / "link-scratch").read_text(), "scratch")
                    self.assertTrue(home.is_dir())
                    if outcome in {"success", "failure"}:
                        finish.touch()
                    elif outcome == "cancel":
                        pathlib.Path(str(log) + shared.ACTION_CANCEL_SUFFIX).touch()
                    elif outcome == "supervisor-term":
                        process.terminate()
                    _, stderr = process.communicate(timeout=5)
                    status = 125 if outcome in {"cancel", "lease-expiry", "supervisor-term"} else 0
                    self.assertEqual(process.returncode, status, stderr)
                    reported = pathlib.Path(str(log) + shared.ACTION_EXIT_FILE_SUFFIX).read_text()
                    self.assertEqual(int(reported), 7 if outcome == "failure" else status)
                    self.assertFalse(action_tmp.exists(), "action scratch survived completion")
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.communicate()
                    if action_tmp is not None:
                        shutil.rmtree(action_tmp, ignore_errors=True)

    def test_run_in_container_falls_back_to_inline_environment_when_env_file_is_unsupported(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            config_path, config = self._write_config(root)
            docker = FakeDocker(
                records={"windows-action-shim": ("", 0)},
                running={wrapper._container_name(config, config_path.parent)},
            )
            docker.env_file_unsupported = True
            os.environ.update({"PATH": "external/toolchain/tools", "SYSTEMROOT": "C:\\Windows"})

            with (
                mock.patch.object(shared, "run", side_effect=docker),
                mock.patch.object(
                    pathlib.Path,
                    "cwd",
                    return_value=pathlib.PureWindowsPath("Z:\\sandbox\\execroot\\_main"),
                ),
                mock.patch("sys.stderr"),
            ):
                exit_code = wrapper._run_in_container(
                    config, config_path, "external/toolchain/tools/lld-link", ["@params"]
                )

            self.assertEqual(exit_code, 0)
            launches = [call for call in docker.commands("exec") if "-d" in call and "-w" in call]
            self.assertEqual(len(launches), 2)
            self.assertIn("--env-file", launches[0])
            self.assertNotIn("--env-file", launches[1])
            self.assertIn("PATH=external/toolchain/tools", launches[1])
            self.assertIn("PWD=/mnt/z/sandbox/execroot/_main", launches[1])
            self.assertNotIn("SYSTEMROOT=C:\\Windows", launches[1])
            cancellations = [
                call
                for call in docker.commands("cp")
                if call[3].endswith(shared.ACTION_CANCEL_SUFFIX)
            ]
            self.assertEqual(len(cancellations), 1)

    def test_run_in_container_restarts_a_missing_container_and_reports_exec_failures(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            # A toolchain on a Windows drive: the runner must make sure the WSL engine's
            # distribution has that drive mounted before it creates the container.
            config_path, config = self._write_config(root, mount_paths=["Z:\\llvm"])
            docker = FakeDocker(
                records={
                    "windows-action-shim": (
                        "sh: external/toolchain/tools/lld-link: not found\n",
                        127,
                    ),
                    "diagnostics": ("working directory: /mnt/z/sandbox\nls: cannot access\n", 0),
                }
            )
            os.environ["DOCKER_HOST"] = "tcp://127.0.0.1:2375"

            with (
                mock.patch.object(shared, "run", side_effect=docker),
                mock.patch.object(wrapper, "_container_path", side_effect=_fake_container_path),
                mock.patch.object(
                    pathlib.Path,
                    "cwd",
                    return_value=pathlib.PurePosixPath("/sandbox/execroot/_main"),
                ),
                mock.patch("sys.stderr") as stderr,
            ):
                exit_code = wrapper._run_in_container(
                    config, config_path, "external/toolchain/tools/lld-link", ["@params"]
                )

            self.assertEqual(exit_code, 127)
            written = "".join(str(call.args[0]) for call in stderr.write.call_args_list)
            self.assertIn("not found", written)
            self.assertIn("Windows container action diagnostics", written)
            self.assertIn("working directory: /mnt/z/sandbox", written)
            runs = docker.commands("run")
            helper_runs = [call for call in runs if "--privileged" in call]
            container_runs = [call for call in runs if "--privileged" not in call]
            # The WSL engine is reached over TCP, so the drives are mounted in its
            # distribution before the container is created.
            self.assertEqual(len(helper_runs), 1)
            self.assertIn("nsenter", helper_runs[0])
            self.assertIn("--privileged", helper_runs[0])
            self.assertEqual(len(container_runs), 1)
            run = container_runs[0]
            self.assertIn("--init", run)
            self.assertIn("/mnt/z:/mnt/z:ro", run)
            self.assertEqual(run[run.index("-w") + 1], "/")
            self.assertIn("-v", run)
            sandbox_mount = (
                f"{_fake_container_path(root / 'sandbox')}:{_fake_container_path(root / 'sandbox')}"
            )
            self.assertIn(sandbox_mount, run)
            shared_mount = (
                f"{_fake_container_path(root / 'shared-install')}:"
                f"{_fake_container_path(root / 'shared-install')}"
            )
            self.assertIn(shared_mount, run)
            self.assertTrue((root / "shared-install").is_dir())
            self.assertEqual(
                run[-3:],
                ["/bin/bash", "-c", "trap 'exit 0' TERM INT; while true; do sleep 3600; done"],
            )
            self.assertEqual(run[run.index("--network") + 1], "host")

    def test_ensure_preflight_container_probes_mounts_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            config_path, config = self._write_config(root, docker_host_mode="wsl")
            probe_commands: list[list[str]] = []

            def probe(command):
                probe_commands.append(command)
                return "present: all\nsymlink resolves\n", 0

            docker = FakeDocker(records={"mount-probe": probe})

            with (
                mock.patch.object(shared, "run", side_effect=docker),
                mock.patch.object(wrapper, "_container_path", side_effect=_fake_container_path),
                mock.patch("sys.stderr"),
            ):
                wrapper.main(["runner", "--ensure-container", str(config_path)])

            self.assertEqual(len(probe_commands), 1)
            probe_args = probe_commands[0][4:]
            output_probe, repo_probe, sandbox_probe, link_probe, expected_target = probe_args
            self.assertTrue(
                output_probe.startswith(_fake_container_path(config_path.parent).as_posix() + "/")
            )
            self.assertTrue(
                repo_probe.startswith(_fake_container_path(root / "repo").as_posix() + "/")
            )
            self.assertTrue(
                sandbox_probe.startswith(_fake_container_path(root / "sandbox").as_posix() + "/")
            )
            self.assertEqual(link_probe, sandbox_probe + ".link")
            self.assertEqual(expected_target, output_probe)
            # No drive-mount helper without the TCP-reached WSL engine.
            self.assertFalse([call for call in docker.commands("run") if "--privileged" in call])
            # Probe files are cleaned up again.
            self.assertEqual(
                [path.name for path in config_path.parent.iterdir()],
                ["mongo_linux_container_actions.json"],
            )
            self.assertEqual(
                sorted(path.name for path in (root / "sandbox").iterdir()),
                [wrapper._RUNNER_SCRATCH_DIRNAME],
            )

    def test_ensure_preflight_container_recreates_once_then_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            config_path, config = self._write_config(root, docker_host_mode="wsl")
            docker = FakeDocker(
                records={
                    "mount-probe": ("MISSING: /mnt/z/output-base/probe\nread-only: /mnt/z\n", 1)
                }
            )

            with (
                mock.patch.object(shared, "run", side_effect=docker),
                mock.patch.object(wrapper, "_container_path", side_effect=_fake_container_path),
                mock.patch("sys.stderr") as stderr,
            ):
                with self.assertRaises(SystemExit) as raised:
                    wrapper.main(["runner", "--ensure-container", str(config_path)])

            self.assertEqual(raised.exception.code, 1)
            written = "".join(str(call.args[0]) for call in stderr.write.call_args_list)
            self.assertIn("recreating it once", written)
            self.assertIn("does not expose the Bazel paths", written)
            self.assertIn("MISSING: /mnt/z/output-base/probe", written)
            self.assertEqual(len([c for c in docker.commands("run") if "--privileged" not in c]), 2)
            self.assertEqual(len(docker.commands("rm")), 1)

    def test_resolve_docker_command_uses_the_recorded_host_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            host_bin = pathlib.Path(temp_dir) / "Program Files" / "Docker"
            host_bin.mkdir(parents=True)
            docker = host_bin / "docker"
            docker.write_text("#!/bin/sh\n", encoding="utf-8")
            docker.chmod(0o755)

            resolved = wrapper._resolve_docker_command(
                self._config(docker_command="docker", host_path=str(host_bin))
            )
            self.assertEqual(resolved, [str(docker)])
            # The resolved command survives a round trip through the config's shlex quoting.
            self.assertEqual(
                wrapper._docker_command({"docker_command": wrapper.shlex.join(resolved)}),
                [str(docker)],
            )
            self.assertEqual(
                wrapper._resolve_docker_command(
                    self._config(
                        docker_command=str(docker)
                        if " " not in str(docker)
                        else wrapper.shlex.quote(str(docker))
                    )
                ),
                [str(docker)],
            )
            # A bare command name that exists nowhere (the recorded host path is empty of
            # it, and no system-wide install can provide this name) resolves to None.
            self.assertIsNone(
                wrapper._resolve_docker_command(
                    self._config(docker_command="definitely_missing_docker_cli", host_path=temp_dir)
                )
            )
            self.assertIsNone(
                wrapper._resolve_docker_command(
                    self._config(docker_command=str(host_bin / "missing.exe"))
                )
            )

    @unittest.skipIf(
        not hasattr(os, "geteuid") or os.geteuid() == 0 or not pathlib.Path("/bin/sh").exists(),
        "needs a POSIX shell and a user whom directory permissions can stop from writing",
    )
    def test_mount_probe_script_survives_the_read_only_check(self):
        """A failed redirection must not abort the probe before the writable/symlink checks."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            read_only_dir = root / "output-base"
            repo_dir = root / "repo"
            writable_dir = root / "sandbox"
            for directory in (read_only_dir, repo_dir, writable_dir):
                directory.mkdir()
            output_probe = read_only_dir / "probe"
            repo_probe = repo_dir / "probe"
            sandbox_probe = writable_dir / "probe"
            link_probe = writable_dir / "probe.link"
            for probe in (output_probe, repo_probe, sandbox_probe):
                probe.write_text("probe\n", encoding="utf-8")
            os.symlink(output_probe, link_probe)
            read_only_dir.chmod(0o555)
            try:
                result = subprocess.run(
                    [
                        "/bin/sh",
                        "-c",
                        wrapper._MOUNT_PROBE_SCRIPT,
                        "mount-probe",
                        str(output_probe),
                        str(repo_probe),
                        str(sandbox_probe),
                        str(link_probe),
                        str(output_probe),
                    ],
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
            finally:
                read_only_dir.chmod(0o755)

            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(f"read-only: {read_only_dir}", result.stdout)
            self.assertIn(f"writable: {writable_dir}", result.stdout)
            self.assertIn(f"symlink resolves: {link_probe}", result.stdout)
            self.assertNotIn("Read-only file system", result.stdout)
            self.assertFalse((writable_dir / "probe.container-write").exists())

            # A missing sandbox probe and a dangling symlink are reported and fail the probe.
            sandbox_probe.unlink()
            os.remove(link_probe)
            os.symlink(root / "does-not-exist", link_probe)
            result = subprocess.run(
                [
                    "/bin/sh",
                    "-c",
                    wrapper._MOUNT_PROBE_SCRIPT,
                    "mount-probe",
                    str(output_probe),
                    str(repo_probe),
                    str(sandbox_probe),
                    str(link_probe),
                    str(output_probe),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn(f"MISSING: {sandbox_probe}", result.stdout)
            self.assertIn("SYMLINK DOES NOT RESOLVE", result.stdout)

    def test_invalid_mount_prefix_is_rejected_before_privileged_commands(self):
        for prefix in ("/mnt;touch", "/mnt/../host", "/mnt/./host", "/"):
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                wrapper._drive_mount_prefix(self._config(drive_mount_prefix=prefix))

    def test_docker_desktop_uses_native_drive_sources(self):
        mounts = wrapper._container_mounts(
            self._config(docker_host_mode="docker-desktop"),
            pathlib.PureWindowsPath("Z:/b/b"),
        )
        self.assertIn("C:\\:/mnt/c:ro", mounts)
        self.assertIn("Z:\\:/mnt/z:ro", mounts)
        self.assertIn("Z:\\b\\b-mongo-action-sandbox:/mnt/z/b/b-mongo-action-sandbox", mounts)

    def test_engine_selection_uses_shared_routing_and_records_selected_command(self):
        config = self._config(docker_command="docker", docker_host_mode="wsl-local")
        with mock.patch.object(
            wrapper.engine,
            "select_linux_docker_command",
            return_value=["wsl.exe", "--exec", "docker"],
        ) as select:
            selected = wrapper._select_engine(config, ["docker"])
        select.assert_called_once_with(["docker"])
        self.assertEqual(selected["docker_command"], "wsl.exe --exec docker")
        self.assertEqual(os.environ[wrapper.engine.DOCKER_HOST_MODE_ENV], "wsl-local")

    def test_wsl_cli_translates_only_client_file_paths(self):
        docker = ["wsl.exe", "--distribution", "Ubuntu", "--exec", "docker"]
        self.assertEqual(
            wrapper._wsl_client_file_args(["cp", "test:/tmp/log", "Z:/output/logs"]),
            ["cp", "test:/tmp/log", "/mnt/z/output/logs"],
        )
        self.assertEqual(
            wrapper._wsl_client_file_args(["cp", "C:/scratch/input", "test:/tmp/input"]),
            ["cp", "/mnt/c/scratch/input", "test:/tmp/input"],
        )
        self.assertEqual(
            wrapper._wsl_client_file_args(
                ["exec", "--env-file", "C:/scratch/action.env", "test", "tool", "Z:/input"]
            ),
            ["exec", "--env-file", "/mnt/c/scratch/action.env", "test", "tool", "Z:/input"],
        )
        command = wrapper._docker_command(self._config(docker_command=" ".join(docker)))
        self.assertIn("--docker-cli", command)
        self.assertEqual(json.loads(command[-2]), docker)
        self.assertEqual(command[-1], "/mnt")

    def test_wsl_cli_uses_configured_drive_mount_prefix(self):
        docker = ["wsl.exe", "--exec", "docker"]
        command = wrapper._docker_command(
            self._config(docker_command=" ".join(docker), drive_mount_prefix="/drives")
        )
        self.assertEqual(command[-1], "/drives")
        with mock.patch.object(
            subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
        ) as run:
            wrapper.main([*command[1:], "cp", "test:/tmp/log", "Z:/output/logs"])
        run.assert_called_once_with(
            [*docker, "cp", "test:/tmp/log", "/drives/z/output/logs"], check=False
        )
        self.assertEqual(
            wrapper._wsl_client_file_args(
                ["exec", "--env-file", "C:/scratch/action.env", "test", "tool"], "/drives"
            ),
            ["exec", "--env-file", "/drives/c/scratch/action.env", "test", "tool"],
        )

    def test_preselected_tcp_engine_is_preserved_for_actions(self):
        config = self._config(
            docker_command="docker",
            engine_selected=True,
            runner_environment={"DOCKER_HOST": "tcp://127.0.0.1:2375"},
        )
        with (
            mock.patch.object(wrapper, "_resolve_docker_command", return_value=["docker"]),
            mock.patch.object(wrapper.engine, "select_linux_docker_command") as select,
        ):
            selected = wrapper.prepare_runner_config(config)
        select.assert_not_called()
        self.assertEqual(selected["docker_command"], "docker")
        self.assertEqual(os.environ["DOCKER_HOST"], "tcp://127.0.0.1:2375")

    def test_preselected_wsl_engine_restores_recovery_mode(self):
        config = self._config(
            docker_command="docker", engine_selected=True, docker_host_mode="wsl-local"
        )
        with (
            mock.patch.object(wrapper, "_resolve_docker_command", return_value=["docker"]),
            mock.patch.object(wrapper.engine.platform, "system", return_value="Windows"),
            mock.patch.object(
                wrapper.engine, "restart_wsl_local_docker_engine", return_value=(True, "")
            ) as restart,
        ):
            wrapper.prepare_runner_config(config)
            start = mock.Mock(side_effect=[SystemExit(125), None])
            self.assertTrue(wrapper.engine.start_container_with_recovery(["docker"], start))
        restart.assert_called_once_with(["docker"])
        self.assertEqual(start.call_count, 2)

    def test_failed_symlink_creation_fails_mount_preflight(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            config_path, config = self._write_config(root, docker_host_mode="wsl")
            docker = FakeDocker(records={"mount-probe": ("present: all\n", 0)}, existing={"test"})
            with (
                mock.patch.object(shared, "run", side_effect=docker),
                mock.patch.object(wrapper, "_container_path", side_effect=_fake_container_path),
                mock.patch.object(os, "symlink", side_effect=OSError("symlinks disabled")),
                mock.patch("sys.stderr"),
            ):
                problems = wrapper._mount_problems(["docker"], config, "test", config_path.parent)
            self.assertIn("symlinks disabled", problems)


if __name__ == "__main__":
    unittest.main()
