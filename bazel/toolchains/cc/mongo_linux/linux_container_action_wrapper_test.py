from __future__ import annotations

import json
import os
import pathlib
import shlex
import signal
import subprocess
import tempfile
import time
import unittest
from unittest import mock

from bazel.toolchains.cc.mongo_linux import linux_container_action_wrapper as wrapper


class LinuxContainerActionWrapperTest(unittest.TestCase):
    @staticmethod
    def _valid_config(**overrides: str) -> dict:
        config = {
            "container_layout_version": "v5",
            "docker_command": "/usr/bin/docker",
            "sandbox_base": "/action-sandbox",
            "shared_install_dir": "/shared-install",
        }
        config.update(overrides)
        return config

    def test_container_shim_signals_action_process_group(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            pidfile = root / "shim.pid"
            groupfile = root / "action.pgid"
            grandchild_pidfile = root / "grandchild.pid"
            grandchild_script = "trap '' TERM; while true; do sleep 300; done"
            action_script = (
                f"trap '' TERM; /bin/bash -c {shlex.quote(grandchild_script)} & "
                f"echo $! > {shlex.quote(str(grandchild_pidfile))}; wait"
            )
            process = subprocess.Popen(
                [
                    "/bin/bash",
                    "-c",
                    wrapper._CONTAINER_SHIM,
                    "container-shim",
                    str(pidfile),
                    str(groupfile),
                    "/bin/bash",
                    "-c",
                    action_script,
                ],
                stderr=subprocess.DEVNULL,
            )
            try:
                self._wait_for_file(pidfile)
                self._wait_for_file(groupfile)
                self._wait_for_file(grandchild_pidfile)

                os.kill(process.pid, signal.SIGTERM)
                time.sleep(0.1)
                self.assertIsNone(process.poll())
                self.assertTrue(pidfile.exists())
                self.assertTrue(groupfile.exists())

                result = subprocess.run(
                    [
                        "/bin/bash",
                        "-c",
                        wrapper._cancellation_command([], "", str(pidfile), str(groupfile), "KILL")[
                            -1
                        ],
                    ],
                    check=False,
                )
                process.wait(timeout=3)
                self.assertEqual(0, result.returncode)
                self._wait_for_process_exit(int(grandchild_pidfile.read_text(encoding="utf-8")))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if groupfile.exists():
                    try:
                        os.killpg(int(groupfile.read_text(encoding="utf-8")), signal.SIGKILL)
                    except (OSError, ValueError):
                        pass

    def test_kill_cancellation_command_signals_action_process_group(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            pidfile = root / "shim.pid"
            groupfile = root / "action.pgid"
            action = subprocess.Popen(["setsid", "--wait", "/bin/bash", "-c", "sleep 300"])
            groupfile.write_text(str(action.pid), encoding="utf-8")
            pidfile.write_text(str(os.getpid()), encoding="utf-8")
            try:
                result = subprocess.run(
                    [
                        "/bin/bash",
                        "-c",
                        wrapper._cancellation_command([], "", str(pidfile), str(groupfile), "KILL")[
                            -1
                        ],
                    ],
                    check=False,
                )
                action.wait(timeout=3)
                self.assertEqual(0, result.returncode)
                self.assertEqual(-signal.SIGKILL, action.returncode)
            finally:
                if action.poll() is None:
                    action.kill()
                    action.wait()

    @staticmethod
    def _wait_for_file(path: pathlib.Path) -> None:
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if path.exists():
                return
            time.sleep(0.01)
        raise AssertionError(f"timed out waiting for {path}")

    @staticmethod
    def _wait_for_process_exit(pid: int) -> None:
        deadline = time.monotonic() + 3
        proc_stat = pathlib.Path(f"/proc/{pid}/stat")
        while time.monotonic() < deadline:
            try:
                stat = proc_stat.read_text(encoding="utf-8")
            except FileNotFoundError:
                return
            state = stat[stat.rfind(")") + 2]
            if state == "Z":
                return
            time.sleep(0.01)
        raise AssertionError(f"process {pid} is still running")

    def test_container_name_extends_configured_name_with_runtime_identity(self) -> None:
        name = wrapper._container_name(
            {"container_name": "mongo_linux_action_configured"},
            pathlib.Path("/output-base"),
        )
        self.assertRegex(name, r"^mongo_linux_action_configured_[0-9a-f]{12}$")

    def test_container_name_changes_with_immutable_runtime_settings(self) -> None:
        output_base = pathlib.Path("/output-base")
        config = {
            "container_name": "mongo_linux_action_configured",
            "container_layout_version": "v5",
            "docker_command": "/usr/bin/docker",
            "home": "/repo/.tmp/home",
            "image": "image@sha256:digest",
            "network": "host",
            "output_base_generation": "generation-one",
            "repo_root": "/repo",
            "sandbox_base": "/sandbox",
            "shared_install_dir": "/shared-install",
            "state_dir": "/repo/.tmp",
            "user": "123:456",
        }
        original = wrapper._container_name(config, output_base)

        replacements = {
            "container_layout_version": "v6",
            "docker_command": "/usr/bin/podman",
            "home": "/other/home",
            "image": "image@sha256:other",
            "network": "none",
            "output_base_generation": "generation-two",
            "repo_root": "/other/repo",
            "sandbox_base": "/other/sandbox",
            "shared_install_dir": "/other/shared-install",
            "state_dir": "/other/state",
            "user": "456:789",
        }
        for key, value in replacements.items():
            with self.subTest(key=key):
                changed = {**config, key: value}
                self.assertNotEqual(original, wrapper._container_name(changed, output_base))

        self.assertNotEqual(
            original,
            wrapper._container_name(config, pathlib.Path("/other-output")),
        )

    def test_trusted_config_dir_uses_reboot_persistent_storage(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            persistent_root = pathlib.Path(temp_dir) / "var-tmp"
            with mock.patch.object(wrapper, "_TRUSTED_CONFIG_ROOT", persistent_root):
                trusted_dir = wrapper._trusted_config_dir()

        self.assertEqual(
            persistent_root / f"{wrapper._TRUSTED_CONFIG_DIR_PREFIX}{os.getuid()}",
            trusted_dir,
        )

    def test_exec_env_forwards_environment_and_replaces_pwd(self) -> None:
        with mock.patch.dict("os.environ", {"PATH": "/tool/bin", "PWD": "/old"}, clear=True):
            args = wrapper._exec_env(
                pathlib.Path("/exec/root"),
                pathlib.Path("/action/tmp"),
                pathlib.Path("/output-base"),
            )
        self.assertEqual(
            [
                "-e",
                "PWD=/exec/root",
                "-e",
                "TMPDIR=/action/tmp",
                "-e",
                "TMP=/action/tmp",
                "-e",
                "TEMP=/action/tmp",
                "-e",
                "HOME=/action/tmp/home",
                "-e",
                "XDG_CACHE_HOME=/action/tmp/home/.cache",
                "-e",
                "XDG_CONFIG_HOME=/action/tmp/home/.config",
                "-e",
                "XDG_DATA_HOME=/action/tmp/home/.local/share",
                "-e",
                "XDG_STATE_HOME=/action/tmp/home/.local/state",
                "-e",
                f"{wrapper._ACTION_CONTAINER_ENV}=1",
                "-e",
                "PATH=/tool/bin",
            ],
            args,
        )

    def test_exec_env_adds_external_libvoidstar_to_library_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            cwd = pathlib.Path(temp_dir)
            libvoidstar_dir = cwd / "external" / "libvoidstar"
            libvoidstar_dir.mkdir(parents=True)
            (libvoidstar_dir / "libvoidstar.so").touch()
            with mock.patch.dict(
                "os.environ",
                {"LD_LIBRARY_PATH": "/toolchain/lib"},
                clear=True,
            ):
                args = wrapper._exec_env(cwd, pathlib.Path("/action/tmp"), cwd / "output-base")

        self.assertIn(
            f"LD_LIBRARY_PATH={libvoidstar_dir}{os.pathsep}/toolchain/lib",
            args,
        )

    def test_exec_env_finds_libvoidstar_in_normal_execroot(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            cwd = root / "action-sandbox" / "execroot" / "_main"
            output_base = root / "output-base"
            repository = output_base / "external" / "_main~_repo_rules~libvoidstar"
            workspace_external = output_base / "execroot" / "_main" / "external"
            cwd.mkdir(parents=True)
            repository.mkdir(parents=True)
            workspace_external.mkdir(parents=True)
            (repository / "libvoidstar.so").touch()
            (workspace_external / "libvoidstar").symlink_to(
                repository,
                target_is_directory=True,
            )
            with mock.patch.dict("os.environ", {}, clear=True):
                args = wrapper._exec_env(cwd, pathlib.Path("/action/tmp"), output_base)

        self.assertIn(
            f"LD_LIBRARY_PATH={workspace_external / 'libvoidstar'}",
            args,
        )

    def test_exec_env_finds_libvoidstar_in_canonical_external_repository(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            cwd = root / "action-sandbox" / "execroot" / "_main"
            output_base = root / "output-base"
            repository = output_base / "external" / "_main~setup_mongo_toolchains~libvoidstar"
            cwd.mkdir(parents=True)
            repository.mkdir(parents=True)
            (repository / "libvoidstar.so").touch()
            with mock.patch.dict("os.environ", {}, clear=True):
                args = wrapper._exec_env(cwd, pathlib.Path("/action/tmp"), output_base)

        self.assertIn(f"LD_LIBRARY_PATH={repository}", args)

    def test_exec_env_replaces_host_temporary_and_home_directories(self) -> None:
        with mock.patch.dict(
            "os.environ",
            {
                "TMPDIR": "/host/tmpdir",
                "TMP": "/host/tmp",
                "TEMP": "/host/temp",
                "HOME": "/host/home",
                "XDG_CACHE_HOME": "/host/cache",
                "XDG_CONFIG_HOME": "/host/config",
                wrapper._ACTION_CONTAINER_ENV: "host-value",
            },
            clear=True,
        ):
            args = wrapper._exec_env(
                pathlib.Path("/exec/root"),
                pathlib.Path("/action/tmp"),
                pathlib.Path("/output-base"),
            )

        self.assertNotIn("TMPDIR=/host/tmpdir", args)
        self.assertNotIn("TMP=/host/tmp", args)
        self.assertNotIn("TEMP=/host/temp", args)
        self.assertNotIn("HOME=/host/home", args)
        self.assertNotIn("XDG_CACHE_HOME=/host/cache", args)
        self.assertNotIn("XDG_CONFIG_HOME=/host/config", args)
        self.assertNotIn(f"{wrapper._ACTION_CONTAINER_ENV}=host-value", args)
        self.assertEqual(args.count("TMPDIR=/action/tmp"), 1)
        self.assertEqual(args.count("TMP=/action/tmp"), 1)
        self.assertEqual(args.count("TEMP=/action/tmp"), 1)
        self.assertEqual(args.count("HOME=/action/tmp/home"), 1)
        self.assertEqual(args.count(f"{wrapper._ACTION_CONTAINER_ENV}=1"), 1)

    def test_podman_runtime_commands_use_private_runtime_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            runtime_dir = pathlib.Path(temp_dir) / "runtime"
            with (
                mock.patch.dict(
                    "os.environ",
                    {"TMPDIR": "/data/mci/task/tmp"},
                    clear=True,
                ),
                mock.patch.object(wrapper, "_podman_runtime_dir", return_value=runtime_dir),
                mock.patch.object(
                    wrapper.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ) as run,
            ):
                wrapper._run(["/usr/bin/podman", "inspect", "boundary"])

            runtime_env = run.call_args.kwargs["env"]
            self.assertEqual(str(runtime_dir), runtime_env["XDG_RUNTIME_DIR"])
            self.assertEqual(str(runtime_dir), runtime_env["TMPDIR"])
            self.assertEqual(str(runtime_dir), runtime_env["TMP"])
            self.assertEqual(str(runtime_dir), runtime_env["TEMP"])
            storage_config = pathlib.Path(runtime_env["CONTAINERS_STORAGE_CONF"])
            self.assertEqual(
                storage_config,
                pathlib.Path(temp_dir)
                / f"mongo-linux-podman-storage-{os.getuid()}"
                / "storage.conf",
            )
            storage_config_text = storage_config.read_text(encoding="utf-8")
            self.assertIn('rootless_storage_path = "', storage_config_text)

    def test_main_restores_podman_task_id_from_trusted_config(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_base = root / "output-base"
            output_base.mkdir()
            config_path = output_base / wrapper._CONFIG_FILENAME
            trusted_dir = root / "trusted"
            trusted_dir.mkdir()
            config = self._valid_config(podman_task_id="current-task-123")
            config_path.write_text(json.dumps(config) + "\n", encoding="utf-8")

            with (
                mock.patch.object(wrapper, "_trusted_config_dir", return_value=trusted_dir),
                mock.patch.object(wrapper, "_run_in_container", return_value=0),
                mock.patch.dict(
                    wrapper.os.environ,
                    {wrapper._PODMAN_TASK_ID_ENV: "stale-task"},
                    clear=True,
                ),
            ):
                wrapper._publish_trusted_config(config_path, config)
                result = wrapper.main(["wrapper", str(config_path), "/real/tool"])
                task_id = wrapper.os.environ.get(wrapper._PODMAN_TASK_ID_ENV)

        self.assertEqual(0, result)
        self.assertEqual("current-task-123", task_id)

    def test_action_uses_host_only_snapshot_after_source_config_is_mutated(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_base = root / "output-base"
            output_base.mkdir()
            config_path = output_base / wrapper._CONFIG_FILENAME
            trusted_dir = root / "trusted"
            trusted_dir.mkdir()
            safe_config = self._valid_config()
            config_path.write_text(json.dumps(safe_config) + "\n", encoding="utf-8")

            with mock.patch.object(wrapper, "_trusted_config_dir", return_value=trusted_dir):
                wrapper._publish_trusted_config(config_path, safe_config)
                config_path.write_text(
                    json.dumps({**safe_config, "docker_command": "/bin/sh -c 'touch /tmp/escaped'"})
                    + "\n",
                    encoding="utf-8",
                )
                with mock.patch.object(wrapper, "_run_in_container", return_value=0) as run:
                    result = wrapper.main(["wrapper", str(config_path), "/real/tool", "--tool-arg"])

        self.assertEqual(0, result)
        self.assertEqual(safe_config, run.call_args.args[0])
        self.assertEqual(pathlib.Path("/real/tool"), run.call_args.args[2])

    def test_action_fails_closed_without_host_only_config_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_base = root / "output-base"
            output_base.mkdir()
            config_path = output_base / wrapper._CONFIG_FILENAME
            trusted_dir = root / "trusted"
            trusted_dir.mkdir()
            config_path.write_text(json.dumps(self._valid_config()) + "\n", encoding="utf-8")

            with (
                mock.patch.object(wrapper, "_trusted_config_dir", return_value=trusted_dir),
                mock.patch.object(wrapper, "_run_in_container") as run,
            ):
                result = wrapper.main(["wrapper", str(config_path), "/real/tool"])

        self.assertEqual(2, result)
        run.assert_not_called()

    def test_ensure_container_publishes_host_only_config_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_base = root / "output-base"
            output_base.mkdir()
            config_path = output_base / wrapper._CONFIG_FILENAME
            trusted_dir = root / "trusted"
            trusted_dir.mkdir()
            config = self._valid_config()
            config_path.write_text(json.dumps(config) + "\n", encoding="utf-8")

            with (
                mock.patch.object(wrapper, "_trusted_config_dir", return_value=trusted_dir),
                mock.patch.object(
                    wrapper,
                    "_ensure_preflight_container",
                    return_value="boundary",
                ),
            ):
                result = wrapper.main(["wrapper", "--ensure-container", str(config_path)])
                trusted = wrapper._read_trusted_config(config_path)

        self.assertEqual(0, result)
        self.assertEqual(config, trusted)

    def test_publish_trusted_config_reuses_identical_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_base = root / "output-base"
            output_base.mkdir()
            config_path = output_base / wrapper._CONFIG_FILENAME
            trusted_dir = root / "trusted"
            trusted_dir.mkdir()
            config = {"docker_command": "/usr/bin/docker"}

            with mock.patch.object(wrapper, "_trusted_config_dir", return_value=trusted_dir):
                destination = wrapper._publish_trusted_config(config_path, config)
                with mock.patch.object(
                    wrapper.os,
                    "open",
                    side_effect=AssertionError("unchanged snapshots must not be rewritten"),
                ):
                    reused = wrapper._publish_trusted_config(config_path, config)

        self.assertEqual(destination, reused)

    def test_read_config_rejects_non_v5_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = pathlib.Path(temp_dir) / "config.json"
            config_path.write_text(
                json.dumps(
                    {
                        **self._valid_config(),
                        "container_layout_version": "legacy",
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "expected v5"):
                wrapper._read_config(config_path)

    def test_start_container_reuses_running_container(self) -> None:
        with (
            mock.patch.object(wrapper, "_container_running", return_value=True),
            mock.patch.object(wrapper, "_container_exists") as exists,
            mock.patch.object(wrapper, "_run") as run,
        ):
            wrapper._start_container(
                ["docker"],
                {"image": "image@sha256:digest"},
                "boundary",
                pathlib.Path("/output-base"),
            )

        exists.assert_not_called()
        run.assert_not_called()

    def test_start_container_restarts_existing_container(self) -> None:
        with (
            mock.patch.object(wrapper, "_container_running", side_effect=[False, True]),
            mock.patch.object(wrapper, "_container_exists", return_value=True),
            mock.patch.object(
                wrapper,
                "_run",
                return_value=subprocess.CompletedProcess([], 0),
            ) as run,
        ):
            wrapper._start_container(
                ["docker"],
                {"image": "image@sha256:digest"},
                "boundary",
                pathlib.Path("/output-base"),
            )

        self.assertEqual(["docker", "start", "boundary"], run.call_args.args[0])

    def test_ensure_container_reuses_running_container_without_writing_state_dir(self) -> None:
        with (
            mock.patch.object(wrapper, "_container_name", return_value="boundary"),
            mock.patch.object(wrapper, "_container_running", return_value=True),
            mock.patch.object(
                wrapper.pathlib.Path,
                "open",
                side_effect=AssertionError("container actions must not write the state directory"),
            ),
        ):
            name = wrapper._ensure_container(
                {
                    "docker_command": "docker",
                    "state_dir": "/read-only/repository/.tmp",
                },
                pathlib.Path("/output-base/mongo_linux_container_actions.json"),
            )

        self.assertEqual("boundary", name)

    def test_preflight_container_executes_inside_running_container(self) -> None:
        with mock.patch.object(
            wrapper,
            "_run",
            return_value=subprocess.CompletedProcess([], 0, stdout="", stderr=""),
        ) as run:
            wrapper._preflight_container(["/usr/bin/podman"], "boundary")

        self.assertEqual(
            ["/usr/bin/podman", "exec", "boundary", "/bin/true"],
            run.call_args.args[0],
        )

    def test_preflight_container_reports_rootless_podman_failure(self) -> None:
        with mock.patch.object(
            wrapper,
            "_run",
            return_value=subprocess.CompletedProcess(
                [],
                125,
                stdout="",
                stderr="newuidmap: write to uid_map failed: Operation not permitted",
            ),
        ):
            with self.assertRaisesRegex(SystemExit, "125"):
                wrapper._preflight_container(["/usr/bin/podman"], "boundary")

    def test_mount_probe_checks_output_base_action_temp_and_sandbox_mounts(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_base = root / "output-base"
            sandbox_base = root / "action-sandbox"
            shared_install_dir = root / "shared-install"
            output_base.mkdir()
            config = {
                "sandbox_base": str(sandbox_base),
                "shared_install_dir": str(shared_install_dir),
            }

            def check_probe(command, **_):
                output_probe = pathlib.Path(command[-7])
                container_temp_probe = pathlib.PurePosixPath(command[-6])
                host_temp_probe = (
                    output_base / wrapper._ACTION_TEMP_DIRNAME / container_temp_probe.name
                )
                sandbox_probe = pathlib.Path(command[-5])
                container_temp_write = pathlib.PurePosixPath(command[-4])
                sandbox_write = pathlib.Path(command[-3])
                shared_install_probe = pathlib.Path(command[-2])
                shared_install_write = pathlib.Path(command[-1])
                self.assertTrue(output_probe.is_file())
                self.assertTrue(host_temp_probe.is_file())
                self.assertTrue(sandbox_probe.is_file())
                self.assertTrue(shared_install_probe.is_file())
                self.assertEqual(container_temp_probe.parent, wrapper._CONTAINER_ACTION_TEMP_ROOT)
                self.assertEqual(container_temp_write.parent, wrapper._CONTAINER_ACTION_TEMP_ROOT)
                self.assertEqual(sandbox_probe.parent, sandbox_base)
                self.assertEqual(sandbox_write.parent, sandbox_base)
                self.assertEqual(shared_install_probe.parent, shared_install_dir)
                self.assertEqual(shared_install_write.parent, shared_install_dir)
                self.assertIn(': > "$4"', command[-9])
                self.assertIn(': > "$5"', command[-9])
                self.assertIn(': > "$7"', command[-9])
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(wrapper, "_run", side_effect=check_probe):
                current = wrapper._container_mounts_current(
                    ["docker"],
                    "boundary",
                    output_base,
                    config,
                )

        self.assertTrue(current)

    def test_preflight_reuses_running_generation_after_mount_write_probe(self) -> None:
        config = self._valid_config(docker_command="docker")
        config_path = pathlib.Path("/output-base") / wrapper._CONFIG_FILENAME
        with (
            mock.patch.object(wrapper, "_container_name", return_value="boundary"),
            mock.patch.object(wrapper, "_container_running", return_value=True),
            mock.patch.object(wrapper, "_ensure_container") as ensure,
            mock.patch.object(wrapper, "_preflight_container") as preflight,
            mock.patch.object(
                wrapper, "_container_mounts_current", return_value=True
            ) as mounts_current,
        ):
            name = wrapper._ensure_preflight_container(config, config_path)

        self.assertEqual("boundary", name)
        ensure.assert_not_called()
        preflight.assert_not_called()
        mounts_current.assert_called_once_with(["docker"], "boundary", config_path.parent, config)

    def test_preflight_starts_and_validates_new_generation(self) -> None:
        config = self._valid_config(docker_command="docker")
        config_path = pathlib.Path("/output-base") / wrapper._CONFIG_FILENAME
        with (
            mock.patch.object(wrapper, "_container_name", return_value="boundary"),
            mock.patch.object(wrapper, "_container_running", return_value=False),
            mock.patch.object(wrapper, "_ensure_container", return_value="boundary") as ensure,
            mock.patch.object(wrapper, "_preflight_container") as preflight,
            mock.patch.object(wrapper, "_container_mounts_current", return_value=True) as mounts,
        ):
            name = wrapper._ensure_preflight_container(config, config_path)

        self.assertEqual("boundary", name)
        ensure.assert_called_once_with(config, config_path)
        preflight.assert_called_once_with(["docker"], "boundary")
        mounts.assert_called_once_with(["docker"], "boundary", config_path.parent, config)

    def test_preflight_recreates_container_with_incorrect_mounts(self) -> None:
        config = self._valid_config(docker_command="docker")
        config_path = pathlib.Path("/output-base") / wrapper._CONFIG_FILENAME
        with (
            mock.patch.object(wrapper, "_container_name", return_value="boundary"),
            mock.patch.object(wrapper, "_container_running", return_value=True),
            mock.patch.object(wrapper, "_ensure_container", return_value="boundary") as ensure,
            mock.patch.object(wrapper, "_preflight_container"),
            mock.patch.object(
                wrapper, "_container_mounts_current", side_effect=[False, True]
            ) as mounts,
            mock.patch.object(wrapper, "_remove_container") as remove,
        ):
            name = wrapper._ensure_preflight_container(config, config_path)

        self.assertEqual("boundary", name)
        remove.assert_called_once_with(["docker"], "boundary")
        ensure.assert_called_once_with(config, config_path)
        self.assertEqual(2, mounts.call_count)

    def test_preflight_fails_closed_when_recreated_container_mounts_are_broken(self) -> None:
        config = self._valid_config(docker_command="docker")
        config_path = pathlib.Path("/output-base") / wrapper._CONFIG_FILENAME
        with (
            mock.patch.object(wrapper, "_container_name", return_value="boundary"),
            mock.patch.object(wrapper, "_container_running", return_value=False),
            mock.patch.object(wrapper, "_ensure_container", return_value="boundary") as ensure,
            mock.patch.object(wrapper, "_preflight_container"),
            mock.patch.object(wrapper, "_container_mounts_current", return_value=False),
            mock.patch.object(wrapper, "_remove_container") as remove,
        ):
            with self.assertRaisesRegex(SystemExit, "1"):
                wrapper._ensure_preflight_container(config, config_path)

        self.assertEqual(2, ensure.call_count)
        remove.assert_called_once_with(["docker"], "boundary")

    def test_start_container_mounts_repo_output_base_and_read_only_config(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo = root / "repo"
            output_base = root / "output-base"
            home = root / "home"
            repo.mkdir()
            output_base.mkdir()
            config = {
                "repo_root": str(repo),
                "home": str(home),
                "image": "image@sha256:digest",
                "network": "standard",
                "sandbox_base": str(root / "action-sandbox"),
                "shared_install_dir": str(root / "shared-install"),
                "user": "123:456",
            }
            commands: list[list[str]] = []

            def record(command, **_):
                commands.append(command)
                return subprocess.CompletedProcess(command, 0)

            with (
                mock.patch.object(wrapper, "_container_running", side_effect=[False, True]),
                mock.patch.object(wrapper, "_container_exists", return_value=False),
                mock.patch.object(wrapper, "_remove_stale_containers"),
                mock.patch.object(wrapper, "_run", side_effect=record),
            ):
                wrapper._start_container(["docker"], config, "boundary", output_base)

        command = commands[0]
        self.assertIn("-w", command)
        self.assertIn(str(repo), command)
        self.assertIn(f"{repo}:{repo}:ro", command)
        self.assertIn(f"{output_base}:{output_base}:ro", command)
        self.assertIn(
            f"{config['sandbox_base']}:{config['sandbox_base']}",
            command,
        )
        self.assertIn(
            f"{config['shared_install_dir']}:{config['shared_install_dir']}",
            command,
        )
        self.assertNotIn(f"{home}:{home}", command)
        config_path = output_base / wrapper._CONFIG_FILENAME
        self.assertIn(f"{config_path}:{config_path}:ro", command)
        self.assertIn(
            f"{output_base / wrapper._ACTION_TEMP_DIRNAME}:{wrapper._CONTAINER_ACTION_TEMP_ROOT}",
            command,
        )
        self.assertIn(f"{wrapper._ACTION_CONTAINER_ENV}=1", command)
        self.assertIn("123:456", command)
        self.assertIn("standard", command)
        self.assertEqual("image@sha256:digest", command[-4])

    def test_start_container_mounts_external_absolute_symlink_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo = root / "repo"
            output_base = root / "output-base"
            external = output_base / "execroot" / "_main" / "external"
            embedded_tools = root / "bazel-install" / "embedded_tools"
            repo.mkdir()
            external.mkdir(parents=True)
            embedded_tools.mkdir(parents=True)
            (external / "bazel_tools").symlink_to(embedded_tools, target_is_directory=True)
            config = {
                "repo_root": str(repo),
                "image": "image@sha256:digest",
                "sandbox_base": str(root / "action-sandbox"),
                "shared_install_dir": str(root / "shared-install"),
            }
            commands: list[list[str]] = []

            def record(command, **_):
                commands.append(command)
                return subprocess.CompletedProcess(command, 0)

            with (
                mock.patch.object(wrapper, "_container_running", side_effect=[False, True]),
                mock.patch.object(wrapper, "_container_exists", return_value=False),
                mock.patch.object(wrapper, "_remove_stale_containers"),
                mock.patch.object(wrapper, "_run", side_effect=record),
            ):
                wrapper._start_container(["docker"], config, "boundary", output_base)

        self.assertIn(f"{embedded_tools}:{embedded_tools}:ro", commands[0])

    def test_start_container_disables_selinux_separation_for_podman_mounts(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo = root / "repo"
            output_base = root / "output-base"
            repo.mkdir()
            output_base.mkdir()
            config = {
                "repo_root": str(repo),
                "home": str(root / "home"),
                "image": "image@sha256:digest",
                "sandbox_base": str(root / "action-sandbox"),
                "shared_install_dir": str(root / "shared-install"),
            }
            commands: list[list[str]] = []

            def record(command, **_):
                commands.append(command)
                return subprocess.CompletedProcess(command, 0)

            with (
                mock.patch.object(wrapper, "_container_running", side_effect=[False, True]),
                mock.patch.object(wrapper, "_container_exists", return_value=False),
                mock.patch.object(wrapper, "_remove_stale_containers"),
                mock.patch.object(wrapper, "_run", side_effect=record),
                mock.patch.object(wrapper.os, "geteuid", return_value=123),
            ):
                wrapper._start_container(["/usr/bin/podman"], config, "boundary", output_base)

        self.assertIn("--security-opt=label=disable", commands[0])
        self.assertIn("--userns=keep-id", commands[0])

    def test_start_container_adopts_matching_container_after_name_conflict(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo = root / "repo"
            output_base = root / "output-base"
            repo.mkdir()
            output_base.mkdir()
            config = {
                "repo_root": str(repo),
                "home": str(root / "home"),
                "image": "image@sha256:digest",
                "sandbox_base": str(root / "action-sandbox"),
                "shared_install_dir": str(root / "shared-install"),
            }
            with (
                mock.patch.object(wrapper, "_container_running", side_effect=[False, True]),
                mock.patch.object(wrapper, "_container_exists", return_value=False),
                mock.patch.object(wrapper, "_remove_stale_containers"),
                mock.patch.object(
                    wrapper,
                    "_run",
                    return_value=subprocess.CompletedProcess([], 125),
                ),
            ):
                wrapper._start_container(["docker"], config, "boundary", output_base)

    def test_start_container_restarts_created_container_after_name_conflict(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo = root / "repo"
            output_base = root / "output-base"
            repo.mkdir()
            output_base.mkdir()
            config = {
                "repo_root": str(repo),
                "home": str(root / "home"),
                "image": "image@sha256:digest",
                "sandbox_base": str(root / "action-sandbox"),
                "shared_install_dir": str(root / "shared-install"),
            }
            responses = [
                subprocess.CompletedProcess([], 125),
                subprocess.CompletedProcess([], 0),
            ]
            with (
                mock.patch.object(
                    wrapper,
                    "_container_running",
                    side_effect=[False, False, True],
                ),
                mock.patch.object(
                    wrapper,
                    "_container_exists",
                    side_effect=[False, True],
                ),
                mock.patch.object(wrapper, "_remove_stale_containers"),
                mock.patch.object(wrapper, "_run", side_effect=responses) as run,
            ):
                wrapper._start_container(["docker"], config, "boundary", output_base)

        self.assertEqual(["docker", "start", "boundary"], run.call_args_list[1].args[0])

    def test_stale_container_replacement_keeps_current_container(self) -> None:
        responses = [
            subprocess.CompletedProcess([], 0, stdout="old\ncurrent\n"),
            subprocess.CompletedProcess([], 0),
        ]
        with mock.patch.object(wrapper, "_run", side_effect=responses) as run:
            wrapper._remove_stale_containers(["docker"], "workspace", "current")
        self.assertEqual(
            ["docker", "rm", "-f", "old"],
            run.call_args_list[1].args[0],
        )
        self.assertEqual(2, run.call_count)

    def test_failure_diagnostics_report_action_paths_to_stderr(self) -> None:
        with mock.patch.object(
            wrapper,
            "_run",
            return_value=subprocess.CompletedProcess([], 0),
        ) as run:
            wrapper._emit_failure_diagnostics(
                ["podman"],
                "boundary",
                pathlib.Path("/exec/root"),
                pathlib.Path("/action/tmp"),
                pathlib.Path("/repo"),
                pathlib.Path("/output-base"),
            )

        command = run.call_args.args[0]
        self.assertEqual(["podman", "exec", "boundary"], command[:3])
        self.assertEqual(
            ["/exec/root", "/action/tmp", "/repo", "/output-base"],
            command[-4:],
        )
        self.assertIs(run.call_args.kwargs["stdout"], wrapper.sys.stderr)
        self.assertIs(run.call_args.kwargs["stderr"], wrapper.sys.stderr)

    def test_run_uses_config_parent_as_output_base_and_keeps_worker_stdio_open(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config_path = root / "output-base" / "mongo_linux_container_actions.json"
            state_dir = root / "state"
            config_path.parent.mkdir()
            process = mock.Mock()
            process.wait.side_effect = lambda: setattr(process, "returncode", 0)
            process.returncode = None
            with (
                mock.patch.object(wrapper, "_container_name", return_value="boundary") as name,
                mock.patch.object(wrapper, "_ensure_container", return_value="boundary") as ensure,
                mock.patch.object(wrapper, "_container_running") as container_running,
                mock.patch.object(wrapper.subprocess, "Popen", return_value=process) as popen,
            ):
                result = wrapper._run_in_container(
                    {
                        "docker_command": "docker",
                        "sandbox_base": str(root / "action-sandbox"),
                        "shared_install_dir": str(root / "shared-install"),
                        "state_dir": str(state_dir),
                    },
                    config_path,
                    pathlib.Path("/real/tool"),
                    ["--persistent_worker"],
                )

        self.assertEqual(0, result)
        name.assert_called_once_with(mock.ANY, config_path.parent)
        ensure.assert_not_called()
        container_running.assert_not_called()
        command = popen.call_args.args[0]
        self.assertEqual(["docker", "exec", "-i"], command[:3])
        self.assertIn("/real/tool", command)
        self.assertIn("--persistent_worker", command)
        self.assertIn(
            f"{wrapper._SHARED_INSTALL_ENV}={root / 'shared-install'}",
            command,
        )
        tmpdir = pathlib.PurePosixPath(
            next(value for value in command if value.startswith("TMPDIR=")).split("=", 1)[1]
        )
        self.assertEqual(wrapper._CONTAINER_ACTION_TEMP_ROOT, tmpdir.parent)
        self.assertLess(
            len(str(tmpdir / "mongo-gpg-12345678" / "S.gpg-agent")),
            108,
        )
        self.assertNotIn(str(config_path.parent), str(tmpdir))
        self.assertFalse(pathlib.Path(tmpdir).exists())
        self.assertEqual({}, popen.call_args.kwargs)

    def test_run_recovers_and_retries_when_exec_finds_no_container(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config_path = root / "output-base" / "mongo_linux_container_actions.json"
            config_path.parent.mkdir()
            failed_process = mock.Mock(returncode=1)
            successful_process = mock.Mock(returncode=0)
            with (
                mock.patch.object(wrapper, "_container_name", return_value="boundary"),
                mock.patch.object(wrapper, "_container_running", return_value=False) as running,
                mock.patch.object(wrapper, "_ensure_container", return_value="boundary") as ensure,
                mock.patch.object(
                    wrapper.subprocess,
                    "Popen",
                    side_effect=[failed_process, successful_process],
                ) as popen,
            ):
                result = wrapper._run_in_container(
                    self._valid_config(docker_command="docker"),
                    config_path,
                    pathlib.Path("/real/tool"),
                    [],
                )

        self.assertEqual(0, result)
        running.assert_called_once_with(["docker"], "boundary")
        ensure.assert_called_once_with(self._valid_config(docker_command="docker"), config_path)
        self.assertEqual(2, popen.call_count)

    def test_run_does_not_retry_a_failed_tool_in_a_running_container(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config_path = root / "output-base" / "mongo_linux_container_actions.json"
            config_path.parent.mkdir()
            config = self._valid_config(docker_command="docker", repo_root=str(root / "repo"))
            process = mock.Mock(returncode=1)
            with (
                mock.patch.object(wrapper, "_container_name", return_value="boundary"),
                mock.patch.object(wrapper, "_container_running", return_value=True) as running,
                mock.patch.object(wrapper, "_ensure_container") as ensure,
                mock.patch.object(wrapper, "_emit_failure_diagnostics") as diagnostics,
                mock.patch.object(wrapper.subprocess, "Popen", return_value=process) as popen,
            ):
                result = wrapper._run_in_container(
                    config,
                    config_path,
                    pathlib.Path("/real/tool"),
                    [],
                )

        self.assertEqual(1, result)
        running.assert_called_once_with(["docker"], "boundary")
        ensure.assert_not_called()
        popen.assert_called_once()
        diagnostics.assert_called_once()

    def test_run_in_container_uses_private_podman_runtime_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            runtime_dir = root / "runtime"
            config_path = root / "output-base" / "mongo_linux_container_actions.json"
            config_path.parent.mkdir()
            process = mock.Mock()
            process.wait.side_effect = lambda: setattr(process, "returncode", 0)
            process.returncode = None
            with (
                mock.patch.dict(
                    "os.environ",
                    {"TMPDIR": "/data/mci/task/tmp"},
                    clear=True,
                ),
                mock.patch.object(wrapper, "_container_name", return_value="boundary"),
                mock.patch.object(wrapper, "_ensure_container", return_value="boundary"),
                mock.patch.object(wrapper, "_podman_runtime_dir", return_value=runtime_dir),
                mock.patch.object(wrapper.subprocess, "Popen", return_value=process) as popen,
            ):
                result = wrapper._run_in_container(
                    {
                        "docker_command": "/usr/bin/podman",
                        "sandbox_base": str(root / "action-sandbox"),
                        "shared_install_dir": str(root / "shared-install"),
                        "state_dir": str(root / "state"),
                    },
                    config_path,
                    pathlib.Path("/real/tool"),
                    [],
                )

        self.assertEqual(0, result)
        runtime_env = popen.call_args.kwargs["env"]
        self.assertEqual(str(runtime_dir), runtime_env["XDG_RUNTIME_DIR"])
        self.assertEqual(str(runtime_dir), runtime_env["TMPDIR"])
        self.assertIn("CONTAINERS_STORAGE_CONF", runtime_env)

    def test_forwarded_cancellation_changes_successful_exit_status(self) -> None:
        handlers: dict[signal.Signals, object] = {}

        def install_handler(signum: signal.Signals, handler: object) -> object:
            previous = handlers.get(signum, signal.SIG_DFL)
            handlers[signum] = handler
            return previous

        process = mock.Mock()
        process.returncode = None
        cancellation_process = mock.Mock()

        def wait() -> None:
            handlers[signal.SIGTERM](signal.SIGTERM, None)
            process.returncode = 0

        process.wait.side_effect = wait
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config_path = root / "output-base" / "mongo_linux_container_actions.json"
            config_path.parent.mkdir()
            with (
                mock.patch.object(wrapper, "_ensure_container", return_value="boundary"),
                mock.patch.object(wrapper, "_container_name", return_value="boundary"),
                mock.patch.object(
                    wrapper,
                    "_start_cancellation_process",
                    return_value=cancellation_process,
                ) as start_cancellation,
                mock.patch.object(wrapper.subprocess, "Popen", return_value=process),
                mock.patch.object(wrapper.signal, "signal", side_effect=install_handler),
            ):
                result = wrapper._run_in_container(
                    {
                        "docker_command": "docker",
                        "sandbox_base": str(root / "action-sandbox"),
                        "shared_install_dir": str(root / "shared-install"),
                        "state_dir": str(root / "state"),
                    },
                    config_path,
                    pathlib.Path("/real/tool"),
                    [],
                )

        self.assertEqual(128 + signal.SIGTERM, result)
        start_cancellation.assert_called_once_with(
            ["docker"], "boundary", mock.ANY, mock.ANY, "TERM"
        )
        cancellation_script = wrapper._cancellation_command(
            ["docker"], "boundary", "/tmp/action.pid", "/tmp/action.pgid", "TERM"
        )[-1]
        self.assertIn("while [[ $attempt -lt 200 ]]", cancellation_script)
        self.assertIn("sleep 0.01", cancellation_script)
        self.assertIn('kill -TERM -- "-$(cat /tmp/action.pgid)"', cancellation_script)
        self.assertIn('kill -TERM "$(cat /tmp/action.pid)"', cancellation_script)
        self.assertLess(
            wrapper._CONTAINER_SHIM.index("trap 'forward_signal TERM 143'"),
            wrapper._CONTAINER_SHIM.index('echo "$$" > "$pidfile"'),
        )
        self.assertIn("setsid -- /bin/bash -c", wrapper._CONTAINER_SHIM)
        self.assertIn('kill -"$1" -- "-$(cat "$groupfile")"', wrapper._CONTAINER_SHIM)

    def test_cancellation_before_container_pidfile_is_published_fails_promptly(self) -> None:
        handlers: dict[signal.Signals, object] = {}

        def install_handler(signum: signal.Signals, handler: object) -> object:
            previous = handlers.get(signum, signal.SIG_DFL)
            handlers[signum] = handler
            return previous

        def cancel_while_starting(*_args) -> mock.Mock:
            handlers[signal.SIGTERM](signal.SIGTERM, None)
            process = mock.Mock(returncode=0)
            return process

        cancellation_process = mock.Mock()
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            config_path = root / "output-base" / wrapper._CONFIG_FILENAME
            config_path.parent.mkdir()
            with (
                mock.patch.object(wrapper, "_container_name", return_value="boundary"),
                mock.patch.object(
                    wrapper,
                    "_start_cancellation_process",
                    return_value=cancellation_process,
                ) as start_cancellation,
                mock.patch.object(
                    wrapper.subprocess, "Popen", side_effect=cancel_while_starting
                ) as popen,
                mock.patch.object(wrapper.signal, "signal", side_effect=install_handler),
            ):
                result = wrapper._run_in_container(
                    self._valid_config(docker_command="docker"),
                    config_path,
                    pathlib.Path("/real/tool"),
                    [],
                )

        self.assertEqual(128 + signal.SIGTERM, result)
        start_cancellation.assert_called_once_with(
            ["docker"], "boundary", mock.ANY, mock.ANY, "TERM"
        )
        popen.assert_called_once()


if __name__ == "__main__":
    unittest.main()
