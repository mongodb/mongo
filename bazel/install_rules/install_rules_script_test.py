import contextlib
import errno
import json
import os
import pathlib
import runpy
import shutil
import sys
import tempfile
import unittest
from collections.abc import Iterator
from unittest import mock


class InstallRulesScriptTest(unittest.TestCase):
    def test_directory_install_dereferences_input_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source"
            source.mkdir()
            source_file = root / "actual-tool"
            source_file.write_text("tool\n", encoding="utf-8")
            source_file.chmod(0o755)
            (source / "tool").symlink_to(source_file)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {str(source): "lib"}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = root / "install-dist-test"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with mock.patch.object(
                sys,
                "argv",
                [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
            ):
                runpy.run_path(str(script), run_name="__main__")

            installed_file = install_dir / "lib" / "source" / "tool"
            self.assertEqual("tool\n", installed_file.read_text(encoding="utf-8"))
            self.assertFalse(installed_file.is_symlink())

    def test_shared_install_directory_survives_action_sandbox(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "test-tool"
            source.parent.mkdir()
            source.write_text("test binary\n", encoding="utf-8")
            source.chmod(0o755)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [str(source)], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = (
                root
                / "sandbox"
                / "execroot"
                / "_main"
                / "bazel-out"
                / "k8-fastbuild"
                / "bin"
                / "install-dist-test"
            )
            shared_install_root = root / "shared-install"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(script),
                        "--depfile",
                        str(depfile),
                        "--install-dir",
                        str(install_dir),
                    ],
                ),
                mock.patch.dict(
                    os.environ,
                    {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(shared_install_root)},
                ),
                mock.patch.object(
                    os,
                    "link",
                    side_effect=OSError(errno.EXDEV, os.strerror(errno.EXDEV)),
                ),
            ):
                runpy.run_path(str(script), run_name="__main__")

            self.assertEqual(
                "test binary\n",
                (install_dir / "bin" / source.name).read_text(encoding="utf-8"),
            )
            self.assertEqual(
                "test binary\n",
                (shared_install_root / "k8-fastbuild" / "bin" / source.name).read_text(
                    encoding="utf-8"
                ),
            )
            self.assertFalse((install_dir.parent / "install").exists())

    def test_shared_install_directory_handles_concurrent_copy_replacement(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "test-tool"
            source.parent.mkdir()
            source.write_text("test binary\n", encoding="utf-8")
            source.chmod(0o755)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [str(source)], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = (
                root
                / "sandbox"
                / "execroot"
                / "_main"
                / "bazel-out"
                / "k8-fastbuild"
                / "bin"
                / "install-dist-test"
            )
            shared_install_root = root / "shared-install"
            shared_destination = shared_install_root / "k8-fastbuild" / "bin" / source.name
            script = pathlib.Path(__file__).with_name("install_rules.py")
            original_copymode = shutil.copymode

            def remove_shared_destination_before_mode_copy(
                src_path: str, dst_path: str, *, follow_symlinks: bool = True
            ) -> None:
                if pathlib.Path(dst_path) == shared_destination:
                    shared_destination.unlink(missing_ok=True)
                original_copymode(
                    src_path,
                    dst_path,
                    follow_symlinks=follow_symlinks,
                )

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(script),
                        "--depfile",
                        str(depfile),
                        "--install-dir",
                        str(install_dir),
                    ],
                ),
                mock.patch.dict(
                    os.environ,
                    {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(shared_install_root)},
                ),
                mock.patch.object(
                    os,
                    "link",
                    side_effect=OSError(errno.EXDEV, os.strerror(errno.EXDEV)),
                ),
                mock.patch.object(
                    shutil,
                    "copymode",
                    side_effect=remove_shared_destination_before_mode_copy,
                ),
            ):
                runpy.run_path(str(script), run_name="__main__")

            self.assertEqual("test binary\n", shared_destination.read_text(encoding="utf-8"))

    def test_shared_install_directory_handles_concurrent_samefile_check(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "test-tool"
            source.parent.mkdir()
            source.write_text("test binary\n", encoding="utf-8")
            source.chmod(0o755)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [str(source)], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = (
                root
                / "sandbox"
                / "execroot"
                / "_main"
                / "bazel-out"
                / "k8-fastbuild"
                / "bin"
                / "install-dist-test"
            )
            shared_install_root = root / "shared-install"
            shared_destination = shared_install_root / "k8-fastbuild" / "bin" / source.name
            shared_destination.parent.mkdir(parents=True)
            shared_destination.write_text("stale binary\n", encoding="utf-8")
            script = pathlib.Path(__file__).with_name("install_rules.py")
            original_samefile = os.path.samefile

            def remove_shared_destination_before_samefile(src_path: str, dst_path: str) -> bool:
                if pathlib.Path(dst_path) == shared_destination:
                    shared_destination.unlink()
                    raise FileNotFoundError(dst_path)
                return original_samefile(src_path, dst_path)

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(script),
                        "--depfile",
                        str(depfile),
                        "--install-dir",
                        str(install_dir),
                    ],
                ),
                mock.patch.dict(
                    os.environ,
                    {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(shared_install_root)},
                ),
                mock.patch.object(
                    os,
                    "link",
                    side_effect=OSError(errno.EXDEV, os.strerror(errno.EXDEV)),
                ),
                mock.patch.object(
                    os.path,
                    "samefile",
                    side_effect=remove_shared_destination_before_samefile,
                ),
            ):
                runpy.run_path(str(script), run_name="__main__")

            self.assertEqual("test binary\n", shared_destination.read_text(encoding="utf-8"))

    def test_shared_install_directory_serializes_directory_replacement(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "mongod.dSYM"
            source_file = (
                source / "Contents" / "Resources" / "Relocations" / "aarch64" / "debug.yml"
            )
            source_file.parent.mkdir(parents=True)
            source_file.write_text("debug info\n", encoding="utf-8")
            source_file.chmod(0o755)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = (
                root
                / "sandbox"
                / "execroot"
                / "_main"
                / "bazel-out"
                / "k8-fastbuild"
                / "bin"
                / "install-dist-test"
            )
            shared_install_root = root / "shared-install"
            shared_destination = shared_install_root / "k8-fastbuild" / "bin" / source.name
            shared_destination.mkdir(parents=True)
            (shared_destination / "stale-file").write_text("stale\n", encoding="utf-8")
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(script),
                        "--depfile",
                        str(depfile),
                        "--install-dir",
                        str(install_dir),
                    ],
                ),
                mock.patch.dict(
                    os.environ,
                    {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(shared_install_root)},
                ),
            ):
                namespace = runpy.run_path(str(script), run_name="__main__")
            script_globals = namespace["install"].__globals__

            lock_held = False
            lock_paths: list[str] = []

            @contextlib.contextmanager
            def tracking_lock(lock_path: str) -> Iterator[None]:
                nonlocal lock_held
                self.assertFalse(lock_held)
                lock_held = True
                lock_paths.append(lock_path)
                try:
                    yield
                finally:
                    lock_held = False

            def is_shared_path(path: str) -> bool:
                return pathlib.Path(path).is_relative_to(shared_install_root)

            original_copy = namespace["_copy_file_atomically"]
            original_rmtree = shutil.rmtree

            def tracking_copy(src_path: str, dst_path: str) -> None:
                if is_shared_path(dst_path):
                    self.assertTrue(lock_held)
                original_copy(src_path, dst_path)

            def tracking_rmtree(path: str, *args, **kwargs) -> None:
                if is_shared_path(path):
                    self.assertTrue(lock_held, f"shared lock was not held; paths: {lock_paths}")
                original_rmtree(path, *args, **kwargs)

            with (
                mock.patch.dict(script_globals, {"_exclusive_file_lock": tracking_lock}),
                mock.patch.dict(script_globals, {"_copy_file_atomically": tracking_copy}),
                mock.patch.object(shutil, "rmtree", side_effect=tracking_rmtree),
            ):
                namespace["install"](str(source), "bin")

            self.assertEqual(
                [str(shared_install_root / "k8-fastbuild.lock")],
                lock_paths,
            )
            self.assertEqual(
                "debug info\n",
                (
                    shared_destination
                    / "Contents"
                    / "Resources"
                    / "Relocations"
                    / "aarch64"
                    / "debug.yml"
                ).read_text(encoding="utf-8"),
            )

    def test_shared_install_directory_isolated_by_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            script = pathlib.Path(__file__).with_name("install_rules.py")
            shared_install_root = root / "shared-install"

            for configuration, contents in (
                ("k8-fastbuild", "fast\n"),
                ("k8-opt", "opt\n"),
            ):
                source = root / configuration / "source" / "test-tool"
                source.parent.mkdir(parents=True)
                source.write_text(contents, encoding="utf-8")
                source.chmod(0o755)
                depfile = root / configuration / "install-deps.json"
                depfile.write_text(
                    json.dumps(
                        {
                            "bins": [str(source)],
                            "libs": [],
                            "roots": {},
                            "includes": {},
                        }
                    ),
                    encoding="utf-8",
                )
                install_dir = (
                    root
                    / configuration
                    / "execroot"
                    / "_main"
                    / "bazel-out"
                    / configuration
                    / "bin"
                    / "install-dist-test"
                )

                with (
                    mock.patch.object(
                        sys,
                        "argv",
                        [
                            str(script),
                            "--depfile",
                            str(depfile),
                            "--install-dir",
                            str(install_dir),
                        ],
                    ),
                    mock.patch.dict(
                        os.environ,
                        {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(shared_install_root)},
                    ),
                ):
                    runpy.run_path(str(script), run_name="__main__")

            self.assertEqual(
                "fast\n",
                (shared_install_root / "k8-fastbuild" / "bin" / "test-tool").read_text(
                    encoding="utf-8"
                ),
            )
            self.assertEqual(
                "opt\n",
                (shared_install_root / "k8-opt" / "bin" / "test-tool").read_text(encoding="utf-8"),
            )


if __name__ == "__main__":
    unittest.main()
