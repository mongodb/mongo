"""Unit tests for bazel.wrapper_hook.hermetic_container.download."""

from __future__ import annotations

import hashlib
import importlib.util
import pathlib
import tempfile
import unittest
from io import BytesIO
from unittest import mock

from bazel.wrapper_hook import hermetic_container_integration

HERMETIC_CONTAINER_PATH = (
    pathlib.Path(__file__).parents[2] / "hermetic_container" / "hermetic_container.py"
)
HERMETIC_CONTAINER_SPEC = importlib.util.spec_from_file_location(
    "hermetic_container_under_test", HERMETIC_CONTAINER_PATH
)
assert HERMETIC_CONTAINER_SPEC is not None
hermetic_container = importlib.util.module_from_spec(HERMETIC_CONTAINER_SPEC)
assert HERMETIC_CONTAINER_SPEC.loader is not None
HERMETIC_CONTAINER_SPEC.loader.exec_module(hermetic_container)


class LinuxBazelDownloadTest(unittest.TestCase):
    def test_download_is_verified_and_atomically_replaces_the_cache(self):
        payload = b"new Linux Bazel"
        expected_sha256 = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as temp_dir:
            destination = pathlib.Path(temp_dir) / "bin" / "bazel"
            destination.parent.mkdir()
            destination.write_bytes(b"previous Linux Bazel")

            with mock.patch.object(
                hermetic_container_integration.urllib.request,
                "urlopen",
                return_value=BytesIO(payload),
            ):
                hermetic_container_integration._download_file(
                    "https://example.invalid/bazel",
                    destination,
                    expected_sha256,
                )

            self.assertEqual(destination.read_bytes(), payload)
            self.assertEqual(
                hermetic_container_integration._linux_bazel_checksum_file(destination).read_text(
                    encoding="utf-8"
                ),
                f"{expected_sha256}\n",
            )
            self.assertTrue(hermetic_container_integration._linux_bazel_cache_is_valid(destination))

    def test_checksum_mismatch_leaves_the_existing_cache_untouched(self):
        expected_sha256 = hashlib.sha256(b"expected Linux Bazel").hexdigest()
        old_contents = b"previous Linux Bazel"
        with tempfile.TemporaryDirectory() as temp_dir:
            destination = pathlib.Path(temp_dir) / "bin" / "bazel"
            destination.parent.mkdir()
            destination.write_bytes(old_contents)

            with (
                mock.patch.object(
                    hermetic_container_integration.urllib.request,
                    "urlopen",
                    return_value=BytesIO(b"corrupt Linux Bazel"),
                ),
                self.assertRaisesRegex(RuntimeError, "does not match the expected SHA-256"),
            ):
                hermetic_container_integration._download_file(
                    "https://example.invalid/bazel",
                    destination,
                    expected_sha256,
                )

            self.assertEqual(destination.read_bytes(), old_contents)
            self.assertFalse(
                hermetic_container_integration._linux_bazel_checksum_file(destination).exists()
            )
            self.assertEqual(list(destination.parent.glob(".bazel.*.tmp")), [])

    def test_resolve_container_bazel_reuses_only_a_verified_cache_entry(self):
        payload = b"cached Linux Bazel"
        expected_sha256 = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            repo_root.joinpath(".bazelversion").write_text("7.5.0\n", encoding="utf-8")
            destination = (
                hermetic_container_integration._hermetic_container_state_dir(repo_root)
                / "bazel"
                / "bazel-7.5.0-linux-x86_64"
                / "bin"
                / "bazel"
            )
            destination.parent.mkdir(parents=True)
            destination.write_bytes(payload)
            hermetic_container_integration._linux_bazel_checksum_file(destination).write_text(
                f"{expected_sha256}\n",
                encoding="utf-8",
            )

            with mock.patch.object(
                hermetic_container_integration.urllib.request,
                "urlopen",
            ) as urlopen:
                command, mounted_binary = hermetic_container_integration._resolve_container_bazel(
                    pathlib.Path("/usr/bin/bazel"),
                    {"MONGO_HERMETIC_CONTAINER_CONTAINER_ARCH": "x86_64"},
                    repo_root,
                    "Darwin",
                )

            self.assertEqual(command, str(destination))
            self.assertEqual(mounted_binary, destination)
            urlopen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
