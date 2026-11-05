import io
import tarfile
import tempfile
import unittest
from pathlib import Path

from buildscripts.package_test_archive import validate_top_level_directory


class PackageTestArchiveTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        self.archive_path = Path(temporary_directory.name) / "mongo-binaries.tgz"

    def write_archive(self, *members: tuple[str, bytes | None]) -> Path:
        with tarfile.open(self.archive_path, "w:gz") as archive:
            for name, contents in members:
                member = tarfile.TarInfo(name)
                if contents is None:
                    member.type = tarfile.DIRTYPE
                    archive.addfile(member)
                else:
                    member.size = len(contents)
                    archive.addfile(member, io.BytesIO(contents))
        return self.archive_path

    def server_members(self, directory: str) -> list[tuple[str, bytes | None]]:
        return [
            (directory, None),
            (f"{directory}/bin/mongod", b"mongod"),
            (f"{directory}/bin/mongos", b"mongos"),
            (f"{directory}/README", b"README"),
        ]

    def test_supported_target_architectures_and_aliases(self) -> None:
        for target_arch, directory_arch in (
            ("x86_64", "x86_64"),
            ("amd64", "x86_64"),
            ("arm64", "aarch64"),
            ("aarch64", "aarch64"),
            ("ppc64le", "ppc64le"),
            ("ppc64el", "ppc64le"),
            ("s390x", "s390x"),
        ):
            for edition_suffix in ("", "-enterprise"):
                with self.subTest(target_arch=target_arch, edition=edition_suffix):
                    directory = f"mongodb-linux-{directory_arch}{edition_suffix}-rhel9-9.0.7"
                    archive = self.write_archive(*self.server_members(directory))
                    validate_top_level_directory(archive, target_arch)

    def test_s390x_archive_rejects_arm64_directory(self) -> None:
        archive = self.write_archive(
            *self.server_members("mongodb-linux-aarch64-enterprise-rhel9-9.0.7")
        )
        with self.assertRaisesRegex(ValueError, "mongodb-linux-s390x-.*aarch64"):
            validate_top_level_directory(archive, "s390x")

    def test_target_architecture_must_match_even_when_it_appears_in_suffix(self) -> None:
        archive = self.write_archive(
            *self.server_members("mongodb-linux-aarch64-enterprise-linux-s390x-9.0.7")
        )
        with self.assertRaisesRegex(ValueError, "mongodb-linux-s390x-"):
            validate_top_level_directory(archive, "s390x")

    def test_target_architecture_requires_a_delimiter(self) -> None:
        archive = self.write_archive(*self.server_members("mongodb-linux-s390xother-rhel9-9.0.7"))
        with self.assertRaisesRegex(ValueError, "mongodb-linux-s390x-"):
            validate_top_level_directory(archive, "s390x")

    def test_multiple_top_level_directories_are_rejected(self) -> None:
        archive = self.write_archive(
            *self.server_members("mongodb-linux-s390x-enterprise-rhel9-9.0.7"),
            ("mongodb-linux-aarch64-enterprise-rhel9-9.0.7/bin/extra", b"extra"),
        )
        with self.assertRaisesRegex(ValueError, "exactly one top-level directory"):
            validate_top_level_directory(archive, "s390x")

    def test_file_outside_directory_is_rejected(self) -> None:
        archive = self.write_archive(
            *self.server_members("mongodb-linux-s390x-enterprise-rhel9-9.0.7"),
            ("README", b"README"),
        )
        with self.assertRaisesRegex(ValueError, "exactly one top-level directory"):
            validate_top_level_directory(archive, "s390x")

    def test_server_binaries_must_be_files_in_bin(self) -> None:
        directory = "mongodb-linux-s390x-enterprise-rhel9-9.0.7"
        for bad_member in (
            (f"{directory}/mongos", b"mongos"),
            (f"{directory}/bin/mongos", None),
        ):
            with self.subTest(member=bad_member):
                archive = self.write_archive((f"{directory}/bin/mongod", b"mongod"), bad_member)
                with self.assertRaisesRegex(ValueError, "Missing.*bin/mongos"):
                    validate_top_level_directory(archive, "s390x")

    def test_archive_does_not_need_explicit_directory_entries(self) -> None:
        directory = "./mongodb-linux-s390x-enterprise-rhel9-9.0.7"
        archive = self.write_archive(
            (f"{directory}/bin/mongod", b"mongod"),
            (f"{directory}/bin/mongos", b"mongos"),
        )
        validate_top_level_directory(archive, "s390x")

    def test_empty_archive_is_rejected(self) -> None:
        archive = self.write_archive()
        with self.assertRaisesRegex(ValueError, "exactly one top-level directory"):
            validate_top_level_directory(archive, "s390x")

    def test_wrong_operating_system_is_rejected(self) -> None:
        archive = self.write_archive(*self.server_members("mongodb-macos-x86_64-9.0.7"))
        with self.assertRaisesRegex(ValueError, "mongodb-linux-x86_64-"):
            validate_top_level_directory(archive, "x86_64")

    def test_paths_must_stay_inside_the_package_directory(self) -> None:
        directory = "mongodb-linux-s390x-enterprise-rhel9-9.0.7"
        for invalid_path in (f"/{directory}/README", f"{directory}/../README"):
            with self.subTest(path=invalid_path):
                archive = self.write_archive(
                    *self.server_members(directory), (invalid_path, b"README")
                )
                with self.assertRaisesRegex(ValueError, "Unexpected archive member path"):
                    validate_top_level_directory(archive, "s390x")

    def test_unsupported_target_architecture_is_rejected(self) -> None:
        archive = self.write_archive(*self.server_members("mongodb-linux-riscv64-rhel9-9.0.7"))
        with self.assertRaisesRegex(ValueError, "Unsupported.*riscv64"):
            validate_top_level_directory(archive, "riscv64")


if __name__ == "__main__":
    unittest.main()
