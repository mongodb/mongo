"""Path policies shared by whole-command containers and action containers."""

from __future__ import annotations

import unittest

from bazel.hermetic_container import container_paths


class ContainerPathsTest(unittest.TestCase):
    def test_destination_drive_case_is_explicit(self):
        path = r"C:\work\mongo"
        self.assertEqual(container_paths.container_path(path), "/C/work/mongo")
        self.assertEqual(
            container_paths.container_path(path, lowercase_drive=True), "/c/work/mongo"
        )

    def test_windows_root_relative_path_keeps_all_components(self):
        self.assertEqual(
            container_paths.container_path(r"\mongo-execroot\bazel-out", windows=True),
            "/mongo-execroot/bazel-out",
        )

    def test_posix_paths_preserve_literal_backslashes(self):
        self.assertEqual(container_paths.container_path(r"/repo/a\b"), r"/repo/a\b")

    def test_wsl_source_accepts_explicit_prefix_without_environment(self):
        self.assertEqual(
            container_paths.wsl_volume_source(r"Z:\output base\repo", "/drives"),
            "/drives/z/output base/repo",
        )
        self.assertEqual(container_paths.wsl_volume_source("/repo", "/mnt"), "/repo")

    def test_wsl_source_rejects_unc_and_drive_relative_paths(self):
        for path in [r"\\server\share\repo", r"C:repo"]:
            with self.subTest(path=path), self.assertRaises(ValueError):
                container_paths.wsl_volume_source(path, "/mnt")

    def test_mount_prefix_rejects_traversal_and_shell_metacharacters(self):
        for prefix in ["/mnt/../tmp", "/mnt/./drives", "/mnt;touch /tmp/oops", "relative", "/"]:
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                container_paths.wsl_volume_source("C:/repo", prefix)

    def test_mount_prefix_accepts_trailing_slashes(self):
        self.assertEqual(
            container_paths.wsl_volume_source("C:/repo", "/mnt/drives///"),
            "/mnt/drives/c/repo",
        )


if __name__ == "__main__":
    unittest.main()
