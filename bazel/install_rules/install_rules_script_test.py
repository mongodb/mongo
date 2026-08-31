import concurrent.futures
import contextlib
import errno
import json
import ntpath
import os
import pathlib
import runpy
import shutil
import signal
import stat
import sys
import tempfile
import threading
import unittest
from collections.abc import Iterator
from unittest import mock


def _temporary_directory() -> tempfile.TemporaryDirectory[str]:
    return tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR"))


class InstallRulesScriptTest(unittest.TestCase):
    def test_windows_install_path_accepts_platform_separator(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with mock.patch.object(
                sys,
                "argv",
                [str(script), "--depfile", str(depfile), "--install-dir", str(root / "install")],
            ):
                namespace = runpy.run_path(str(script), run_name="__main__")

            with (
                mock.patch.object(namespace["os"], "name", "nt"),
                mock.patch.object(namespace["os"], "path", ntpath),
            ):
                self.assertEqual(
                    r"bin\mongod.exe",
                    namespace["_install_relative_path"]("mongod.exe", "bin", False),
                )

    def test_root_file_install_allows_empty_folder(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "test-list.txt"
            source.parent.mkdir()
            source.write_text("test\n", encoding="utf-8")
            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {str(source): ""}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = root / "install-dist-test"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with mock.patch.object(
                sys,
                "argv",
                [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
            ):
                namespace = runpy.run_path(str(script), run_name="__main__")

            self.assertEqual("test\n", (install_dir / source.name).read_text(encoding="utf-8"))
            self.assertEqual(str(install_dir.parent / "install"), namespace["install_link"])

    def test_macos_default_shared_install_is_outside_output_tree(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = (
                root
                / "output-base"
                / "execroot"
                / "_main"
                / "bazel-out"
                / "darwin_arm64-dbg"
                / "bin"
                / "install-mongor-debug"
            )
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
                ),
                mock.patch.object(sys, "platform", "darwin"),
                mock.patch.object(tempfile, "gettempdir", return_value=str(root / "system-temp")),
                mock.patch.dict(os.environ, {"MONGO_BAZEL_SHARED_INSTALL_DIR": ""}),
            ):
                namespace = runpy.run_path(str(script), run_name="__main__")

            expected = (
                root / "system-temp" / "output-base-mongo-shared-install" / "darwin_arm64-dbg"
            )
            try:
                self.assertEqual(str(expected), namespace["install_link"])
                convenience_link = install_dir.parent / "install"
                self.assertTrue(convenience_link.is_symlink())
                self.assertEqual(expected, convenience_link.resolve())
            finally:
                shutil.rmtree(expected.parent, ignore_errors=True)

    def test_linux_default_shared_install_is_outside_output_tree(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = (
                root
                / "output-base"
                / "execroot"
                / "_main"
                / "bazel-out"
                / "k8-fastbuild"
                / "bin"
                / "install-dist-test"
            )
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
                ),
                mock.patch.object(sys, "platform", "linux"),
                mock.patch.object(tempfile, "gettempdir", return_value=str(root / "system-temp")),
                mock.patch.dict(os.environ, {"MONGO_BAZEL_SHARED_INSTALL_DIR": ""}),
            ):
                namespace = runpy.run_path(str(script), run_name="__main__")

            expected = root / "system-temp" / "output-base-mongo-shared-install" / "k8-fastbuild"
            self.assertEqual(str(expected), namespace["install_link"])
            convenience_link = install_dir.parent / "install"
            self.assertTrue(convenience_link.is_symlink())
            self.assertEqual(expected, convenience_link.resolve())

    def test_stable_output_symlink_is_hardlinked(self) -> None:
        if os.name == "nt":
            self.skipTest("Windows may not support hardlinks to symlinks")

        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            action_output_base = root / "sandbox" / "execroot" / "_main"
            stable_output_base = root / "stable" / "execroot" / "_main"
            source_directory = action_output_base / "bazel-out" / "k8-fastbuild" / "bin" / "src"
            source_directory.mkdir(parents=True)
            stable_source_directory = (
                stable_output_base / "bazel-out" / "k8-fastbuild" / "bin" / "src"
            )
            stable_source_directory.mkdir(parents=True)
            target = stable_source_directory / "tool_with_debug"
            target.write_text("tool\n", encoding="utf-8")
            source = source_directory / "tool"
            source.symlink_to(target)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [str(source)], "libs": [], "roots": {}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = (
                action_output_base / "bazel-out" / "k8-fastbuild" / "bin" / "install-dist-test"
            )
            shared_install_root = root / "shared-install"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
                ),
                mock.patch.dict(
                    os.environ,
                    {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(shared_install_root)},
                ),
            ):
                runpy.run_path(str(script), run_name="__main__")

            installed = install_dir / "bin" / source.name
            shared = shared_install_root / "k8-fastbuild" / "bin" / source.name
            source_inode = os.stat(source, follow_symlinks=False)
            self.assertTrue(installed.is_symlink())
            self.assertTrue(shared.is_symlink())
            self.assertEqual(source_inode.st_ino, os.stat(installed, follow_symlinks=False).st_ino)
            self.assertEqual(source_inode.st_ino, os.stat(shared, follow_symlinks=False).st_ino)
            self.assertEqual(3, source_inode.st_nlink)

            original_link = os.link

            def fail_shared_hardlink(src_path: str, dst_path: str, *args, **kwargs) -> None:
                if str(dst_path).startswith(str(shared_install_root)):
                    raise OSError(errno.EXDEV, os.strerror(errno.EXDEV))
                original_link(src_path, dst_path, *args, **kwargs)

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
                ),
                mock.patch.dict(
                    os.environ,
                    {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(shared_install_root)},
                ),
                mock.patch.object(os, "link", side_effect=fail_shared_hardlink),
            ):
                runpy.run_path(str(script), run_name="__main__")

            source_inode = os.stat(source, follow_symlinks=False)
            self.assertTrue(installed.is_symlink())
            self.assertTrue(shared.is_symlink())
            self.assertEqual(source_inode.st_ino, os.stat(installed, follow_symlinks=False).st_ino)
            self.assertNotEqual(source_inode.st_ino, os.stat(shared, follow_symlinks=False).st_ino)
            self.assertEqual(str(target), os.readlink(shared))

    def test_directory_install_dereferences_input_symlinks(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source"
            source.mkdir()
            source_file = root / "actual-tool"
            source_file.write_text("tool\n", encoding="utf-8")
            source_file.chmod(0o755)
            (source / "tool").symlink_to(source_file)
            linked_directory = root / "actual-directory"
            linked_directory.mkdir()
            (linked_directory / "data").write_text("directory data\n", encoding="utf-8")
            (source / "linked-directory").symlink_to(linked_directory, target_is_directory=True)

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
            installed_directory = install_dir / "lib" / "source" / "linked-directory"
            self.assertTrue(installed_directory.is_dir())
            self.assertFalse(installed_directory.is_symlink())
            self.assertEqual(
                "directory data\n",
                (installed_directory / "data").read_text(encoding="utf-8"),
            )

    def test_directory_install_hardlinks_regular_files(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source"
            source.mkdir()
            source_file = source / "tool"
            source_file.write_text("tool\n", encoding="utf-8")
            source_file.chmod(0o755)

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

            self.assertTrue(os.path.samefile(source_file, install_dir / "lib" / "source" / "tool"))
            self.assertTrue(
                os.path.samefile(
                    source_file,
                    install_dir.parent / "install" / "lib" / "source" / "tool",
                )
            )

    def test_directory_install_handles_long_destination_paths(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source"
            source.mkdir()
            install_dir = root / "install-dist-test"
            destination_prefix = install_dir / "lib" / "source"
            target_path_length = 240
            filename_length = target_path_length - len(str(destination_prefix)) - 1
            if filename_length < 16:
                self.skipTest("test temporary directory leaves insufficient path-length headroom")
            source_file = source / ("long-data-file-" + "x" * (filename_length - 15))
            source_file.write_text("long path data\n", encoding="utf-8")

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {str(source): "lib"}, "includes": {}}),
                encoding="utf-8",
            )
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with mock.patch.object(
                sys,
                "argv",
                [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
            ):
                runpy.run_path(str(script), run_name="__main__")

            installed_file = destination_prefix / source_file.name
            self.assertEqual("long path data\n", installed_file.read_text(encoding="utf-8"))

    def test_repeat_directory_install_preserves_source_mode(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source"
            source.mkdir()
            source_file = source / "data"
            source_file.write_text("data\n", encoding="utf-8")
            source_file.chmod(0o640)
            source_mode = stat.S_IMODE(source_file.stat().st_mode)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps({"bins": [], "libs": [], "roots": {str(source): "lib"}, "includes": {}}),
                encoding="utf-8",
            )
            install_dir = root / "install-dist-test"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            for iteration in range(2):
                with mock.patch.object(
                    sys,
                    "argv",
                    [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
                ):
                    runpy.run_path(str(script), run_name="__main__")

                self.assertEqual(source_mode, stat.S_IMODE(source_file.stat().st_mode))
                if iteration == 0:
                    # A repeat install must still clean read-only output directories without
                    # chmodding their hardlinked files (and therefore the source files).
                    (install_dir / "lib" / "source").chmod(0o555)
                    (install_dir / "lib").chmod(0o555)
                    install_dir.chmod(0o555)

    def test_duplicate_depfiles_install_each_entry_once(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "test-tool"
            source.parent.mkdir()
            source.write_text("test binary\n", encoding="utf-8")

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps(
                    {
                        "bins": [],
                        "libs": [str(source)],
                        "roots": {},
                        # This is the same logical destination through a different manifest
                        # category. Destination ownership, rather than the raw tuple, deduplicates
                        # it and the repeated depfile below.
                        "includes": {str(source): f"lib/{source.name}"},
                    }
                ),
                encoding="utf-8",
            )
            install_dir = root / "install-dist-test"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(script),
                        "--depfile",
                        str(depfile),
                        "--depfile",
                        str(depfile),
                        "--install-dir",
                        str(install_dir),
                    ],
                ),
                mock.patch.object(
                    os,
                    "link",
                    side_effect=OSError(errno.EXDEV, os.strerror(errno.EXDEV)),
                ),
                mock.patch.object(shutil, "copyfile", wraps=shutil.copyfile) as copyfile,
            ):
                runpy.run_path(str(script), run_name="__main__")

            # One copy materializes the action output and one stages the shared output. Passing
            # the same depfile twice must not repeat either operation.
            self.assertEqual(2, copyfile.call_count)

    def test_conflicting_sources_for_one_destination_fail(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            sources = []
            for directory, contents in (("first", "first\n"), ("second", "second\n")):
                source = root / directory / "test-tool"
                source.parent.mkdir()
                source.write_text(contents, encoding="utf-8")
                sources.append(source)

            depfile = root / "install-deps.json"
            depfile.write_text(
                json.dumps(
                    {
                        "bins": [str(source) for source in sources],
                        "libs": [],
                        "roots": {},
                        "includes": {},
                    }
                ),
                encoding="utf-8",
            )
            install_dir = root / "install-dist-test"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
                ),
                self.assertRaisesRegex(RuntimeError, "multiple sources"),
            ):
                runpy.run_path(str(script), run_name="__main__")

    def test_install_destination_cannot_escape_tree(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source"
            source.write_text("data\n", encoding="utf-8")
            depfile = root / "install-deps.json"
            install_dir = root / "install-dist-test"
            script = pathlib.Path(__file__).with_name("install_rules.py")

            for destination in (
                "../escaped",
                str(root / "absolute-escaped"),
                "C:/escaped",
                "lib\\escaped",
            ):
                with self.subTest(destination=destination):
                    depfile.write_text(
                        json.dumps(
                            {
                                "bins": [],
                                "libs": [],
                                "roots": {},
                                "includes": {str(source): destination},
                            }
                        ),
                        encoding="utf-8",
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
                        self.assertRaisesRegex(RuntimeError, "Invalid|escapes"),
                    ):
                        runpy.run_path(str(script), run_name="__main__")

            self.assertFalse((root / "escaped").exists())
            self.assertFalse((root / "absolute-escaped").exists())

    def test_shared_install_directory_survives_action_sandbox(self) -> None:
        with _temporary_directory() as temp_dir:
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
            shared_destination.symlink_to(source)
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
                shared_destination.read_text(encoding="utf-8"),
            )
            self.assertFalse(shared_destination.is_symlink())
            source.unlink()
            self.assertEqual("test binary\n", shared_destination.read_text(encoding="utf-8"))
            self.assertFalse((install_dir.parent / "install").exists())

    def test_shared_install_reopens_readonly_publication_directory(self) -> None:
        if os.name == "nt":
            self.skipTest("POSIX directory permissions are required for this regression test")

        with _temporary_directory() as temp_dir:
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
            install_link = root / "sandbox-mongo-shared-install" / "k8-fastbuild"
            shared_destination = install_link / "bin" / source.name
            shared_destination.parent.mkdir(parents=True)
            install_link.chmod(0o555)
            shared_destination.parent.chmod(0o555)
            script = pathlib.Path(__file__).with_name("install_rules.py")

            try:
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
                        {
                            "MONGO_BAZEL_SHARED_INSTALL_DIR": str(
                                root / "sandbox-mongo-shared-install"
                            )
                        },
                    ),
                ):
                    runpy.run_path(str(script), run_name="__main__")
            finally:
                # Restore permissions so TemporaryDirectory can remove the simulated shared tree.
                shared_destination.parent.chmod(0o755)
                install_link.chmod(0o755)

            self.assertEqual("test binary\n", shared_destination.read_text(encoding="utf-8"))

    def test_shared_install_reopens_readonly_staging_directory(self) -> None:
        if os.name == "nt":
            self.skipTest("POSIX directory permissions are required for this regression test")

        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
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
            script = pathlib.Path(__file__).with_name("install_rules.py")
            with mock.patch.object(
                sys,
                "argv",
                [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
            ):
                namespace = runpy.run_path(str(script), run_name="__main__")

            staged_parent = root / "staging"
            staged_parent.mkdir()
            staged = staged_parent / "new"
            staged.mkdir()
            (staged / "binary").write_text("new binary\n", encoding="utf-8")
            destination_parent = root / "destination"
            destination_parent.mkdir()
            destination = destination_parent / "binary"

            try:
                staged_parent.chmod(0o555)
                staged.chmod(0o555)
                namespace["_replace_staged_destination"](str(staged), str(destination))
            finally:
                staged_parent.chmod(0o755)

            self.assertEqual("new binary\n", (destination / "binary").read_text(encoding="utf-8"))

    def test_shared_install_does_not_traverse_action_output_directory(self) -> None:
        if os.name == "nt":
            self.skipTest("POSIX directory permissions are required for this regression test")

        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "test-tool"
            source.parent.mkdir()
            source.write_text("test binary\n", encoding="utf-8")
            source.chmod(0o755)

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
            shared_destination = (
                root / "sandbox-mongo-shared-install" / "k8-fastbuild" / "bin" / source.name
            )
            script = pathlib.Path(__file__).with_name("install_rules.py")

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    [str(script), "--depfile", str(depfile), "--install-dir", str(install_dir)],
                ),
                mock.patch.dict(
                    os.environ,
                    {"MONGO_BAZEL_SHARED_INSTALL_DIR": str(root / "sandbox-mongo-shared-install")},
                ),
            ):
                namespace = runpy.run_path(str(script), run_name="__main__")

            def make_action_output_inaccessible(_src: str, _dst: str) -> None:
                install_dir.chmod(0)

            try:
                with mock.patch.dict(
                    namespace["install"].__globals__,
                    {"_install_destination": make_action_output_inaccessible},
                ):
                    namespace["install"](str(source), "bin")
            finally:
                install_dir.chmod(0o755)

            self.assertEqual("test binary\n", shared_destination.read_text(encoding="utf-8"))

    def test_shared_install_restores_previous_destination_when_publish_fails(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            source = root / "source" / "test-tool"
            source.parent.mkdir()
            source.write_text("test binary\n", encoding="utf-8")
            source.chmod(0o755)

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
            staging_root = pathlib.Path(str(shared_install_root / "k8-fastbuild") + ".staging")
            shared_destination.parent.mkdir(parents=True)
            shared_destination.write_text("previous binary\n", encoding="utf-8")
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
            original_replace = os.replace

            def fail_staged_publish(src_path: str, dst_path: str) -> None:
                source_path = pathlib.Path(src_path)
                if (
                    pathlib.Path(dst_path) == shared_destination
                    and source_path.name == "new"
                    and source_path.parent.name.startswith(".stage-")
                ):
                    raise OSError(errno.EIO, os.strerror(errno.EIO))
                original_replace(src_path, dst_path)

            with mock.patch.object(os, "replace", side_effect=fail_staged_publish):
                with self.assertRaises(OSError):
                    namespace["install"](str(source), "bin")

            self.assertEqual("previous binary\n", shared_destination.read_text(encoding="utf-8"))
            self.assertEqual([], list(staging_root.glob(".stage-*")))

            original_populate = namespace["_populate_destination"]

            def terminate_after_staging(src_path: str, dst_path: str) -> None:
                original_populate(src_path, dst_path)
                handler = signal.getsignal(signal.SIGTERM)
                self.assertTrue(callable(handler))
                handler(signal.SIGTERM, None)

            with (
                mock.patch.dict(
                    script_globals,
                    {"_populate_destination": terminate_after_staging},
                ),
                self.assertRaises(SystemExit) as termination,
                namespace["_termination_as_exception"](),
            ):
                namespace["install"](str(source), "bin")

            self.assertEqual(128 + signal.SIGTERM, termination.exception.code)
            self.assertEqual("previous binary\n", shared_destination.read_text(encoding="utf-8"))
            self.assertEqual([], list(staging_root.glob(".stage-*")))

            def interrupt_staged_publish(src_path: str, dst_path: str) -> None:
                source_path = pathlib.Path(src_path)
                if (
                    pathlib.Path(dst_path) == shared_destination
                    and source_path.name == "new"
                    and source_path.parent.name.startswith(".stage-")
                ):
                    raise KeyboardInterrupt
                original_replace(src_path, dst_path)

            with mock.patch.object(os, "replace", side_effect=interrupt_staged_publish):
                with self.assertRaises(KeyboardInterrupt):
                    namespace["install"](str(source), "bin")

            self.assertEqual("previous binary\n", shared_destination.read_text(encoding="utf-8"))
            self.assertEqual([], list(staging_root.glob(".stage-*")))

            def fail_publish_and_rollback(src_path: str, dst_path: str) -> None:
                source_path = pathlib.Path(src_path)
                if pathlib.Path(dst_path) == shared_destination:
                    if source_path.name == "new":
                        raise OSError(errno.EIO, os.strerror(errno.EIO))
                    if source_path.name == "old":
                        raise OSError(errno.EPERM, os.strerror(errno.EPERM))
                original_replace(src_path, dst_path)

            with mock.patch.object(os, "replace", side_effect=fail_publish_and_rollback):
                with self.assertRaisesRegex(RuntimeError, "recovery data remains"):
                    namespace["install"](str(source), "bin")

            recovery_workspaces = list(staging_root.glob(".stage-*"))
            self.assertEqual(1, len(recovery_workspaces))
            self.assertEqual(
                "previous binary\n",
                (recovery_workspaces[0] / "old").read_text(encoding="utf-8"),
            )

    def test_shared_install_directory_handles_concurrent_samefile_check(self) -> None:
        with _temporary_directory() as temp_dir:
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

    def test_shared_install_stages_directory_outside_publication_lock(self) -> None:
        with _temporary_directory() as temp_dir:
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
            publication_renames: list[tuple[pathlib.Path, pathlib.Path]] = []
            removed_previous_destination = False

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

            original_populate = namespace["_populate_destination"]
            original_remove = namespace["_remove_destination"]
            original_replace = os.replace

            def tracking_populate(src_path: str, dst_path: str) -> None:
                if is_shared_path(dst_path):
                    self.assertFalse(lock_held)
                original_populate(src_path, dst_path)

            def tracking_replace(src_path: str, dst_path: str) -> None:
                if is_shared_path(src_path) or is_shared_path(dst_path):
                    self.assertTrue(lock_held, f"shared lock was not held; paths: {lock_paths}")
                    publication_renames.append((pathlib.Path(src_path), pathlib.Path(dst_path)))
                original_replace(src_path, dst_path)

            def tracking_remove(path: str) -> None:
                nonlocal removed_previous_destination
                if is_shared_path(path):
                    self.assertFalse(lock_held)
                    removed_previous_destination = True
                original_remove(path)

            with (
                mock.patch.dict(script_globals, {"_exclusive_file_lock": tracking_lock}),
                mock.patch.dict(script_globals, {"_populate_destination": tracking_populate}),
                mock.patch.dict(script_globals, {"_remove_destination": tracking_remove}),
                mock.patch.object(os, "replace", side_effect=tracking_replace),
            ):
                namespace["install"](str(source), "bin")

            self.assertEqual(
                [str(shared_install_root / "k8-fastbuild.lock")],
                lock_paths,
            )
            self.assertEqual(2, len(publication_renames))
            self.assertTrue(removed_previous_destination)
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

    def test_disjoint_shared_install_staging_runs_in_parallel(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            sources = []
            for name in ("first.dSYM", "second.dSYM"):
                source = root / "source" / name
                source.mkdir(parents=True)
                (source / "debug.yml").write_text(name + "\n", encoding="utf-8")
                sources.append(source)

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
            original_populate = namespace["_populate_destination"]
            staging_barrier = threading.Barrier(2)

            def synchronized_populate(src_path: str, dst_path: str) -> None:
                destination = pathlib.Path(dst_path)
                if destination.name == "new" and destination.parent.name.startswith(".stage-"):
                    staging_barrier.wait(timeout=5)
                original_populate(src_path, dst_path)

            with mock.patch.dict(script_globals, {"_populate_destination": synchronized_populate}):
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                    futures = [
                        executor.submit(namespace["install"], str(source), "bin")
                        for source in sources
                    ]
                    for future in futures:
                        future.result(timeout=10)

            for source in sources:
                self.assertEqual(
                    source.name + "\n",
                    (
                        shared_install_root / "k8-fastbuild" / "bin" / source.name / "debug.yml"
                    ).read_text(encoding="utf-8"),
                )

    def test_concurrent_same_destination_publication_is_coherent(self) -> None:
        with _temporary_directory() as temp_dir:
            root = pathlib.Path(temp_dir)
            sources = []
            for version in ("first", "second"):
                source = root / version / "mongod.dSYM"
                source.mkdir(parents=True)
                for index in range(20):
                    (source / f"debug-{index}.yml").write_text(
                        version + "\n",
                        encoding="utf-8",
                    )
                sources.append(source)

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
            shared_destination = shared_install_root / "k8-fastbuild" / "bin" / "mongod.dSYM"
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
            script_globals = namespace["_install_shared_destination"].__globals__
            original_populate = namespace["_populate_destination"]
            staging_barrier = threading.Barrier(2)

            def synchronized_populate(src_path: str, dst_path: str) -> None:
                original_populate(src_path, dst_path)
                staging_barrier.wait(timeout=5)

            with mock.patch.dict(script_globals, {"_populate_destination": synchronized_populate}):
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                    futures = [
                        executor.submit(
                            namespace["_install_shared_destination"],
                            str(source),
                            str(shared_destination),
                        )
                        for source in sources
                    ]
                    for future in futures:
                        future.result(timeout=10)

            installed_contents = {
                file.read_text(encoding="utf-8") for file in shared_destination.iterdir()
            }
            self.assertIn(installed_contents, ({"first\n"}, {"second\n"}))
            self.assertEqual(20, len(list(shared_destination.iterdir())))

    def test_shared_install_directory_isolated_by_configuration(self) -> None:
        with _temporary_directory() as temp_dir:
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
