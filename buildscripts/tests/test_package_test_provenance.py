import json
import os
import tarfile
import tempfile
import types
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from buildscripts import generate_bazel_spawn_pb2
from buildscripts import package_test_provenance as under_test


def release_local_suffix() -> str:
    return under_test.RELEASE_LOCAL_SAFETY_FLAGS


def workspace_file_path(relative_path: str) -> Path:
    test_srcdir = os.environ.get("TEST_SRCDIR")
    test_workspace = os.environ.get("TEST_WORKSPACE")
    if test_srcdir and test_workspace:
        candidate = Path(test_srcdir) / test_workspace / relative_path
        if candidate.exists():
            return candidate

    return Path(__file__).resolve().parents[2] / relative_path


def encode_varint(value: int) -> bytes:
    encoded = bytearray()
    while value > 0x7F:
        encoded.append((value & 0x7F) | 0x80)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def protobuf_varint(field_number: int, value: int) -> bytes:
    return encode_varint((field_number << 3) | 0) + encode_varint(value)


def protobuf_string(field_number: int, value: str) -> bytes:
    encoded = value.encode("utf-8")
    return encode_varint((field_number << 3) | 2) + encode_varint(len(encoded)) + encoded


def protobuf_message(field_number: int, value: bytes) -> bytes:
    return encode_varint((field_number << 3) | 2) + encode_varint(len(value)) + value


def delimited_message(value: bytes) -> bytes:
    return encode_varint(len(value)) + value


def compact_log_with_spawns(*spawns: tuple[str, bool]) -> bytes:
    invocation_entry = protobuf_message(2, protobuf_string(1, "SHA-256"))
    entries = [delimited_message(invocation_entry)]
    for index, (runner, cache_hit) in enumerate(spawns):
        spawn = (
            protobuf_string(7, f"//src:mongod_{index}")
            + protobuf_string(8, "CppLink")
            + protobuf_string(11, runner)
            + protobuf_varint(12, int(cache_hit))
        )
        entries.append(delimited_message(protobuf_message(7, spawn)))
    return b"".join(entries)


class PackageTestProvenanceTest(unittest.TestCase):
    def test_generator_bazel_version_matches_workspace_bazelversion(self):
        bazel_version = workspace_file_path(".bazelversion").read_text().strip()
        self.assertEqual(generate_bazel_spawn_pb2.MONGODB_BAZEL_VERSION, bazel_version)

    def test_valid_command_with_final_suffix_passes(self):
        under_test.validate_release_build_command(
            f"bazel build --config=evg archive-dist-stripped {release_local_suffix()}"
        )

    def test_valid_command_with_separate_config_value_passes(self):
        under_test.validate_release_build_command(
            f"bazel build --config public-release archive-dist-stripped {release_local_suffix()}"
        )

    def test_missing_suffix_fails(self):
        with self.assertRaisesRegex(ValueError, "required release-local safety suffix"):
            under_test.validate_release_build_command(
                "bazel build --config=evg archive-dist-stripped"
            )

    def test_same_flags_present_earlier_but_not_final_fails(self):
        with self.assertRaisesRegex(ValueError, "required release-local safety suffix"):
            under_test.validate_release_build_command(
                f"bazel build {release_local_suffix()} --keep_going archive-dist-stripped"
            )

    def test_target_before_suffix_passes(self):
        under_test.validate_release_build_command(
            f"bazel build archive-dist-stripped {release_local_suffix()}"
        )

    def test_later_remote_executor_override_fails(self):
        with self.assertRaisesRegex(ValueError, "required release-local safety suffix"):
            under_test.validate_release_build_command(
                f"bazel build {release_local_suffix()} "
                "--remote_executor=grpcs://example.invalid archive-dist-stripped"
            )

    def test_earlier_no_cache_removal_followed_by_final_no_cache_addition_passes(self):
        under_test.validate_release_build_command(
            "bazel build --modify_execution_info=.*=-no-cache "
            f"archive-dist-stripped {release_local_suffix()}"
        )

    def test_fetch_release_binary_provenance_selects_provenance_artifact(self):
        task = types.SimpleNamespace(
            display_name="package",
            task_id="task_id",
            artifacts=[
                types.SimpleNamespace(name="Binaries", url="https://example.invalid/binaries.tgz"),
                types.SimpleNamespace(
                    name="Release Binary Provenance",
                    url="https://example.invalid/provenance.json",
                ),
            ],
        )
        response = mock.Mock()
        response.text = "{}"
        response.raise_for_status = mock.Mock()

        with mock.patch.object(under_test.requests, "get", return_value=response) as get_mock:
            provenance = under_test.fetch_release_binary_provenance_from_tasks([task], "package")

        self.assertEqual(response.text, provenance)
        get_mock.assert_called_once_with("https://example.invalid/provenance.json", timeout=300)
        response.raise_for_status.assert_called_once()

    def test_fetch_release_execution_log_selects_execution_log_artifact(self):
        task = types.SimpleNamespace(
            display_name="package",
            task_id="task_id",
            artifacts=[
                types.SimpleNamespace(
                    name="Release Execution Log",
                    url="https://example.invalid/release_execution_log.binpb.zst",
                ),
            ],
        )
        response = mock.Mock()
        response.iter_content.return_value = [b"log bytes"]
        response.raise_for_status = mock.Mock()

        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "release_execution_log.binpb.zst"
            with mock.patch.object(under_test.requests, "get", return_value=response) as get_mock:
                actual_path = under_test.fetch_release_execution_log_from_tasks(
                    [task], "package", output_path
                )

            self.assertEqual(output_path, actual_path)
            self.assertEqual(b"log bytes", output_path.read_bytes())

        get_mock.assert_called_once_with(
            "https://example.invalid/release_execution_log.binpb.zst", stream=True, timeout=300
        )
        response.raise_for_status.assert_called_once()

    def test_compact_execution_log_with_local_runner_passes(self):
        summary = under_test.validate_compact_execution_log_bytes(
            compact_log_with_spawns(("linux-sandbox", False))
        )

        self.assertEqual(1, summary.spawn_count)
        self.assertEqual({"linux-sandbox": 1}, summary.runner_counts)

    def test_compact_execution_log_rejects_remote_runner(self):
        with self.assertRaisesRegex(ValueError, "remote execution/cache"):
            under_test.validate_compact_execution_log_bytes(
                compact_log_with_spawns(("remote", False))
            )

    def test_compact_execution_log_rejects_remote_cache_hit_runner(self):
        with self.assertRaisesRegex(ValueError, "remote execution/cache"):
            under_test.validate_compact_execution_log_bytes(
                compact_log_with_spawns(("remote cache hit", True))
            )

    def test_compact_execution_log_allows_disk_cache_hit_runner(self):
        summary = under_test.validate_compact_execution_log_bytes(
            compact_log_with_spawns(("disk cache hit", True))
        )

        self.assertEqual(1, summary.cache_hit_count)
        self.assertEqual({"disk cache hit": 1}, summary.runner_counts)

    def test_compact_execution_log_rejects_log_with_no_spawns(self):
        with self.assertRaisesRegex(ValueError, "contains no spawn entries"):
            under_test.validate_compact_execution_log_bytes(compact_log_with_spawns())

    def test_compact_execution_log_allows_empty_log_when_requested(self):
        summary = under_test.validate_compact_execution_log_bytes(
            compact_log_with_spawns(), allow_empty=True
        )

        self.assertEqual(0, summary.spawn_count)
        self.assertEqual({}, summary.runner_counts)

    def test_combined_execution_log_summaries_require_non_empty_total(self):
        empty_summary = under_test.validate_compact_execution_log_bytes(
            compact_log_with_spawns(), allow_empty=True
        )

        with self.assertRaisesRegex(ValueError, "contain no spawn entries"):
            under_test.combine_execution_log_validation_summaries([empty_summary])

    def test_combined_execution_log_summaries_add_counts(self):
        empty_summary = under_test.validate_compact_execution_log_bytes(
            compact_log_with_spawns(), allow_empty=True
        )
        local_summary = under_test.validate_compact_execution_log_bytes(
            compact_log_with_spawns(("linux-sandbox", False), ("disk cache hit", True))
        )

        summary = under_test.combine_execution_log_validation_summaries(
            [empty_summary, local_summary]
        )

        self.assertEqual(2, summary.spawn_count)
        self.assertEqual(1, summary.cache_hit_count)
        self.assertEqual({"disk cache hit": 1, "linux-sandbox": 1}, summary.runner_counts)

    def test_release_binary_provenance_validates_hashes_and_build_id(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            bin_dir = temp_path / "bin"
            bin_dir.mkdir()
            (bin_dir / "mongod").write_text("mongod bytes")
            (bin_dir / "mongos").write_text("mongos bytes")

            provenance = {
                "build_id": "build_id",
                "build_command": f"bazel build archive-dist-stripped {release_local_suffix()}",
                "binaries": under_test.hash_release_binaries_in_directory(bin_dir),
            }

            under_test.validate_release_binary_provenance(
                json.dumps(provenance), bin_dir, expected_build_id="build_id"
            )

    def test_release_binary_provenance_accepts_windows_exe_binaries(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            bin_dir = temp_path / "bin"
            bin_dir.mkdir()
            (bin_dir / "mongod.exe").write_text("mongod bytes")
            (bin_dir / "mongos.exe").write_text("mongos bytes")

            provenance = {
                "build_id": "build_id",
                "build_command": f"bazel build archive-dist-stripped {release_local_suffix()}",
                "binaries": under_test.hash_release_binaries_in_directory(bin_dir),
            }

            under_test.validate_release_binary_provenance(
                json.dumps(provenance), bin_dir, expected_build_id="build_id"
            )
            self.assertEqual("mongod.exe", Path(provenance["binaries"]["mongod"]["path"]).name)
            self.assertEqual("mongos.exe", Path(provenance["binaries"]["mongos"]["path"]).name)

    def test_release_binary_name_accepts_crypt_shared_library(self):
        for filename in (
            "mongodb/lib/mongo_crypt_v1.so",
            "mongodb/lib/mongo_crypt_v1.dylib",
            "mongodb/lib/mongo_crypt_v1.dll",
        ):
            with self.subTest(filename=filename):
                self.assertEqual(
                    "mongo_crypt_v1.so",
                    under_test.release_binary_name_from_path(
                        filename, under_test.CRYPT_RELEASE_BINARY_NAMES
                    ),
                )

    def test_release_binary_provenance_rejects_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            bin_dir = temp_path / "bin"
            bin_dir.mkdir()
            (bin_dir / "mongod").write_text("mongod bytes")
            (bin_dir / "mongos").write_text("mongos bytes")

            provenance = {
                "build_command": f"bazel build archive-dist-stripped {release_local_suffix()}",
                "binaries": under_test.hash_release_binaries_in_directory(bin_dir),
            }
            (bin_dir / "mongod").write_text("different mongod bytes")

            with self.assertRaisesRegex(ValueError, "Release binary hash mismatch for mongod"):
                under_test.validate_release_binary_provenance(json.dumps(provenance), bin_dir)

    def test_release_binary_provenance_rejects_build_id_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            bin_dir = temp_path / "bin"
            bin_dir.mkdir()
            (bin_dir / "mongod").write_text("mongod bytes")
            (bin_dir / "mongos").write_text("mongos bytes")

            provenance = {
                "build_id": "other_build_id",
                "build_command": f"bazel build archive-dist-stripped {release_local_suffix()}",
                "binaries": under_test.hash_release_binaries_in_directory(bin_dir),
            }

            with self.assertRaisesRegex(ValueError, "build_id does not match"):
                under_test.validate_release_binary_provenance(
                    json.dumps(provenance), bin_dir, expected_build_id="build_id"
                )

    def test_make_release_binary_provenance_hashes_archive_binaries(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                f"bazel build archive-dist-stripped {release_local_suffix()}"
            )
            archive_path = temp_path / "dist.tgz"
            mongod = temp_path / "mongod"
            mongos = temp_path / "mongos"
            mongod.write_text("mongod bytes")
            mongos.write_text("mongos bytes")
            with tarfile.open(archive_path, "w:gz") as archive:
                archive.add(mongod, arcname="mongodb/bin/mongod")
                archive.add(mongos, arcname="mongodb/bin/mongos")

            provenance = under_test.make_release_binary_provenance(build_command_file, archive_path)

            self.assertEqual(
                under_test.hash_file(mongod), provenance["binaries"]["mongod"]["sha256"]
            )
            self.assertEqual(
                under_test.hash_file(mongos), provenance["binaries"]["mongos"]["sha256"]
            )

    def test_make_release_binary_provenance_hashes_zip_exe_binaries(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                f"bazel build archive-dist-stripped {release_local_suffix()}"
            )
            archive_path = temp_path / "dist.zip"
            mongod = temp_path / "mongod.exe"
            mongos = temp_path / "mongos.exe"
            mongod.write_text("mongod bytes")
            mongos.write_text("mongos bytes")
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.write(mongod, arcname="mongodb/bin/mongod.exe")
                archive.write(mongos, arcname="mongodb/bin/mongos.exe")

            provenance = under_test.make_release_binary_provenance(build_command_file, archive_path)

            self.assertEqual(
                under_test.hash_file(mongod), provenance["binaries"]["mongod"]["sha256"]
            )
            self.assertEqual(
                under_test.hash_file(mongos), provenance["binaries"]["mongos"]["sha256"]
            )
            self.assertEqual("mongodb/bin/mongod.exe", provenance["binaries"]["mongod"]["path"])
            self.assertEqual("mongodb/bin/mongos.exe", provenance["binaries"]["mongos"]["path"])

    def test_make_release_binary_provenance_hashes_crypt_archive_binary(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                f"bazel build archive-mongo_crypt-stripped {release_local_suffix()}"
            )
            archive_path = temp_path / "mongo_crypt.tgz"
            crypt_library = temp_path / "mongo_crypt_v1.so"
            crypt_library.write_text("crypt bytes")
            with tarfile.open(archive_path, "w:gz") as archive:
                archive.add(crypt_library, arcname="mongodb/lib/mongo_crypt_v1.so")

            provenance = under_test.make_release_binary_provenance(
                build_command_file, archive_path, under_test.CRYPT_RELEASE_BINARY_NAMES
            )

            self.assertEqual(
                under_test.hash_file(crypt_library),
                provenance["binaries"]["mongo_crypt_v1.so"]["sha256"],
            )

    def test_make_release_binary_provenance_hashes_crypt_dylib_archive_binary(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                f"bazel build archive-mongo_crypt-stripped {release_local_suffix()}"
            )
            archive_path = temp_path / "mongo_crypt.tgz"
            crypt_library = temp_path / "mongo_crypt_v1.dylib"
            crypt_library.write_text("crypt bytes")
            with tarfile.open(archive_path, "w:gz") as archive:
                archive.add(crypt_library, arcname="mongodb/lib/mongo_crypt_v1.dylib")

            provenance = under_test.make_release_binary_provenance(
                build_command_file, archive_path, under_test.CRYPT_RELEASE_BINARY_NAMES
            )

            self.assertEqual(
                under_test.hash_file(crypt_library),
                provenance["binaries"]["mongo_crypt_v1.so"]["sha256"],
            )
            self.assertEqual(
                "mongodb/lib/mongo_crypt_v1.dylib",
                provenance["binaries"]["mongo_crypt_v1.so"]["path"],
            )

    def test_make_release_binary_provenance_hashes_crypt_dll_zip_archive_binary(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                f"bazel build archive-mongo_crypt-stripped {release_local_suffix()}"
            )
            archive_path = temp_path / "mongo_crypt.zip"
            crypt_library = temp_path / "mongo_crypt_v1.dll"
            crypt_library.write_text("crypt bytes")
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.write(crypt_library, arcname="mongodb/bin/mongo_crypt_v1.dll")

            provenance = under_test.make_release_binary_provenance(
                build_command_file, archive_path, under_test.CRYPT_RELEASE_BINARY_NAMES
            )

            self.assertEqual(
                under_test.hash_file(crypt_library),
                provenance["binaries"]["mongo_crypt_v1.so"]["sha256"],
            )
            self.assertEqual(
                "mongodb/bin/mongo_crypt_v1.dll",
                provenance["binaries"]["mongo_crypt_v1.so"]["path"],
            )

    def test_release_binary_provenance_validates_against_archive(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            archive_path = temp_path / "mongo_crypt.tgz"
            crypt_library = temp_path / "mongo_crypt_v1.so"
            crypt_library.write_text("crypt bytes")
            with tarfile.open(archive_path, "w:gz") as archive:
                archive.add(crypt_library, arcname="mongodb/lib/mongo_crypt_v1.so")

            provenance = {
                "build_id": "build_id",
                "build_command": f"bazel build archive-mongo_crypt-stripped {release_local_suffix()}",
                "binaries": under_test.hash_release_binaries_in_archive(
                    archive_path, under_test.CRYPT_RELEASE_BINARY_NAMES
                ),
            }

            under_test.validate_release_binary_provenance_against_archive(
                json.dumps(provenance),
                archive_path,
                expected_build_id="build_id",
                binary_names=under_test.CRYPT_RELEASE_BINARY_NAMES,
            )

    def test_create_provenance_file_rejects_missing_execution_log_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                f"bazel build archive-dist-stripped {release_local_suffix()}"
            )
            archive_path = temp_path / "dist.tgz"
            mongod = temp_path / "mongod"
            mongos = temp_path / "mongos"
            mongod.write_text("mongod bytes")
            mongos.write_text("mongos bytes")
            with tarfile.open(archive_path, "w:gz") as archive:
                archive.add(mongod, arcname="mongodb/bin/mongod")
                archive.add(mongos, arcname="mongodb/bin/mongos")

            with self.assertRaisesRegex(RuntimeError, "Release execution log file does not exist"):
                under_test.create_provenance_file(
                    build_command_file,
                    archive_path,
                    temp_path / "release_binary_provenance.json",
                    temp_path / "missing_execution_log.binpb.zst",
                )

    def test_create_provenance_file_skips_ordinary_patch_build(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                "bazel build --config=public-release archive-dist-stripped"
            )
            output_file = temp_path / "release_binary_provenance.json"
            output_file.write_text("stale provenance")

            with mock.patch.dict(
                "os.environ", {"is_patch": "true", "is_release": "false"}, clear=False
            ):
                under_test.create_provenance_file(
                    build_command_file, temp_path / "missing-dist.tgz", output_file
                )

            self.assertFalse(output_file.exists())

    def test_create_provenance_file_can_skip_non_release_safe_build_command(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text("bazel build archive-mongo_crypt-stripped")
            output_file = temp_path / "release_binary_provenance.json"
            output_file.write_text("stale provenance")

            under_test.create_provenance_file(
                build_command_file,
                temp_path / "missing-crypt.tgz",
                output_file,
                binary_names=under_test.CRYPT_RELEASE_BINARY_NAMES,
                skip_if_build_command_not_release_safe=True,
            )

            self.assertFalse(output_file.exists())

    def test_create_provenance_file_skips_package_task_outside_server_release_project(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            build_command_file = temp_path / "build_command.txt"
            build_command_file.write_text(
                "bazel build --config=public-release archive-dist-stripped"
            )
            output_file = temp_path / "release_binary_provenance.json"
            output_file.write_text("stale provenance")

            with mock.patch.dict(
                "os.environ",
                {
                    "is_patch": "false",
                    "is_release": "false",
                    "project": "mongodb-mongo-master-nightly",
                    "task_name": "package",
                },
                clear=False,
            ):
                under_test.create_provenance_file(
                    build_command_file, temp_path / "missing-dist.tgz", output_file
                )

            self.assertFalse(output_file.exists())

    def test_archive_dist_test_can_create_provenance_outside_server_release_project(self):
        with mock.patch.dict(
            "os.environ",
            {
                "is_patch": "false",
                "is_release": "false",
                "project": "mongodb-mongo-master-nightly",
                "task_name": "archive_dist_test",
            },
            clear=False,
        ):
            self.assertTrue(under_test.should_create_release_binary_provenance())

    def test_package_task_can_create_provenance_in_server_release_project(self):
        with mock.patch.dict(
            "os.environ",
            {
                "is_patch": "false",
                "is_release": "false",
                "project": "mongo-release",
                "task_name": "package",
            },
            clear=False,
        ):
            self.assertTrue(under_test.should_create_release_binary_provenance())

    def test_package_task_can_create_provenance_in_prefixed_server_release_project(self):
        for project in ("mongo-release-v8.0", "mongo-release-testing"):
            with self.subTest(project=project):
                with mock.patch.dict(
                    "os.environ",
                    {
                        "is_patch": "false",
                        "is_release": "false",
                        "project": project,
                        "task_name": "package",
                    },
                    clear=False,
                ):
                    self.assertTrue(under_test.should_create_release_binary_provenance())


if __name__ == "__main__":
    unittest.main()
