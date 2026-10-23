import gzip
import hashlib
import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

from buildscripts.package_test import package_test_internal as under_test


class PackageTestInternalHelpersTest(unittest.TestCase):
    def test_installed_release_binaries_match_archive_hashes(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            binary_dir = pathlib.Path(temporary_directory)
            hashes = {}
            for name in ("mongod", "mongos"):
                contents = (name + " release binary").encode()
                (binary_dir / name).write_bytes(contents)
                hashes[name] = hashlib.sha256(contents).hexdigest()

            under_test.validate_installed_release_binaries(hashes, binary_dir)
            for name in hashes:
                with self.subTest(binary=name):
                    original = (binary_dir / name).read_bytes()
                    (binary_dir / name).write_bytes(original + b"changed metadata")
                    with self.assertRaisesRegex(ValueError, "hash mismatch for " + name):
                        under_test.validate_installed_release_binaries(hashes, binary_dir)
                    (binary_dir / name).write_bytes(original)

    def test_installed_release_binaries_accept_custom_binary_names(self):
        for names in (("mongod",), ("mongos",), ("mongod", "mongos", "mongocryptd")):
            with (
                self.subTest(names=names),
                tempfile.TemporaryDirectory() as temporary_directory,
            ):
                binary_dir = pathlib.Path(temporary_directory)
                hashes = {}
                for name in names:
                    contents = (name + " release binary").encode()
                    (binary_dir / name).write_bytes(contents)
                    hashes[name] = hashlib.sha256(contents).hexdigest()

                under_test.validate_installed_release_binaries(hashes, binary_dir)
                changed_binary = names[-1]
                with (binary_dir / changed_binary).open("ab") as binary:
                    binary.write(b"changed metadata")
                with self.assertRaisesRegex(ValueError, "hash mismatch for " + changed_binary):
                    under_test.validate_installed_release_binaries(hashes, binary_dir)

    def test_installed_release_binaries_require_nonempty_hashes(self):
        with self.assertRaisesRegex(ValueError, "at least one binary hash"):
            under_test.validate_installed_release_binaries({})

    def test_installed_release_binaries_match_custom_names_case_insensitively(self):
        for filename in ("mongod", "mongos", "mongo_crypt_v1.so", "CustomBinary"):
            with (
                self.subTest(filename=filename),
                tempfile.TemporaryDirectory() as temporary_directory,
            ):
                binary_dir = pathlib.Path(temporary_directory)
                binary = binary_dir / filename
                contents = b"finalized release binary"
                binary.write_bytes(contents)
                selected_name = filename.swapcase()
                hashes = {selected_name: hashlib.sha256(contents).hexdigest()}
                under_test.validate_installed_release_binaries(hashes, binary_dir)

                binary.write_bytes(contents + b"modified")
                with self.assertRaisesRegex(ValueError, "hash mismatch for " + selected_name):
                    under_test.validate_installed_release_binaries(hashes, binary_dir)

    def test_installed_release_binaries_reject_ambiguous_case_insensitive_match(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            binary_dir = pathlib.Path(temporary_directory)
            for filename in ("mongod", "MongoD"):
                (binary_dir / filename).write_bytes(b"release binary")
            with self.assertRaisesRegex(ValueError, "Ambiguous installed release binary name"):
                under_test.validate_installed_release_binaries({"MONGOD": "0" * 64}, binary_dir)

    def test_installed_release_binaries_reject_missing_binary(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            with self.assertRaises(FileNotFoundError):
                under_test.validate_installed_release_binaries(
                    {"mongod": "0" * 64, "mongos": "0" * 64}, pathlib.Path(temporary_directory)
                )

    def test_package_install_checks_hashes_before_running_binaries(self):
        for package_manager in ("apt", "yum", "zypper"):
            with self.subTest(package_manager=package_manager):
                hashes = {"mongod": "0" * 64, "mongos": "1" * 64}
                calls = mock.Mock()
                with (
                    mock.patch.object(
                        sys, "argv", ["test.py", "test.log", "--edition", "org", "packages.tgz"]
                    ),
                    mock.patch.dict(
                        os.environ, {"MONGO_RELEASE_BINARY_HASHES": json.dumps(hashes)}
                    ),
                    mock.patch.object(under_test, "configure_logging"),
                    mock.patch.object(
                        under_test, "download_extract_all_packages", return_value=["server.pkg"]
                    ),
                    mock.patch.object(
                        under_test,
                        "run_and_log",
                        side_effect=lambda command, **kwargs: mock.Mock(
                            returncode=0 if command.startswith(package_manager) else 1
                        ),
                    ),
                    mock.patch.object(
                        under_test, "run_" + package_manager + "_test", calls.install
                    ),
                    mock.patch.object(
                        under_test, "validate_installed_release_binaries", calls.validate
                    ),
                    mock.patch.object(under_test, "get_test_args", calls.get_args),
                ):
                    calls.validate.side_effect = ValueError("hash mismatch for mongod")
                    with self.assertRaisesRegex(ValueError, "hash mismatch for mongod"):
                        under_test.main()
                    self.assertEqual(
                        [mock.call.install(packages=["server.pkg"]), mock.call.validate(hashes)],
                        calls.mock_calls,
                    )

    def test_crypt_package_install_validates_library_hash(self):
        for package_manager in ("apt", "yum", "zypper"):
            for library_state in ("matching", "modified", "missing"):
                with (
                    self.subTest(package_manager=package_manager, library_state=library_state),
                    tempfile.TemporaryDirectory() as temporary_directory,
                ):
                    library_dir = pathlib.Path(temporary_directory)
                    contents = b"finalized crypt library"
                    hashes = {"mongo_crypt_v1.so": hashlib.sha256(contents).hexdigest()}

                    def install(packages):
                        if library_state != "missing":
                            (library_dir / "mongo_crypt_v1.so").write_bytes(
                                contents if library_state == "matching" else contents + b"modified"
                            )

                    with (
                        mock.patch.object(
                            sys,
                            "argv",
                            [
                                "test.py",
                                "test.log",
                                "--edition",
                                "enterprise",
                                "--package-kind",
                                "crypt_v1",
                                "packages.tgz",
                            ],
                        ),
                        mock.patch.dict(
                            os.environ,
                            {"MONGO_RELEASE_BINARY_HASHES": json.dumps(hashes)},
                            clear=True,
                        ),
                        mock.patch.object(under_test, "configure_logging"),
                        mock.patch.object(
                            under_test, "download_extract_all_packages", return_value=["crypt.pkg"]
                        ),
                        mock.patch.object(
                            under_test,
                            "run_and_log",
                            side_effect=lambda command, **kwargs: mock.Mock(
                                returncode=0 if command.startswith(package_manager) else 1
                            ),
                        ),
                        mock.patch.object(
                            under_test, "run_" + package_manager + "_test", side_effect=install
                        ),
                        mock.patch.object(
                            under_test,
                            "get_package_library_dir",
                            return_value=str(library_dir),
                        ) as get_library_dir,
                        mock.patch.object(
                            under_test, "get_test_args", return_value={"package_kind": "crypt_v1"}
                        ) as get_args,
                        mock.patch.object(under_test, "run_server_package_tests") as check_server,
                        mock.patch.object(under_test, "test_install_is_complete") as check_install,
                        mock.patch.object(under_test, "test_uninstall"),
                        mock.patch.object(under_test, "test_uninstall_is_complete"),
                    ):
                        if library_state == "matching":
                            self.assertEqual(0, under_test.main())
                            check_install.assert_called_once()
                        else:
                            error = ValueError if library_state == "modified" else FileNotFoundError
                            with self.assertRaises(error):
                                under_test.main()
                            get_args.assert_not_called()
                            check_install.assert_not_called()
                        get_library_dir.assert_called_once_with(package_manager)
                        check_server.assert_not_called()

    def test_package_library_dir_uses_rpm_macro_on_rpm_systems(self):
        for package_manager in ("yum", "zypper"):
            with (
                self.subTest(package_manager=package_manager),
                mock.patch.object(
                    under_test, "run_and_log", return_value=mock.Mock(stdout=b"/usr/lib64\n")
                ) as run,
            ):
                self.assertEqual("/usr/lib64", under_test.get_package_library_dir(package_manager))
                run.assert_called_once_with("rpm --eval '%{_libdir}'")

        with mock.patch.object(under_test, "run_and_log") as run:
            self.assertEqual("/usr/lib", under_test.get_package_library_dir("apt"))
            run.assert_not_called()

    def test_writes_compressed_text_file(self):
        contents = "source_one.cpp\nsource_two.h\n"
        with tempfile.TemporaryDirectory() as temporary_directory:
            output_path = pathlib.Path(temporary_directory) / "sources.txt.gz"

            under_test.write_compressed_text_file(output_path, contents)

            with gzip.open(str(output_path), "rt", encoding="utf-8") as output_file:
                self.assertEqual(contents, output_file.read())

    def test_detects_server_package_sets(self):
        package_names = [
            "mongodb-enterprise-unstable",
            "mongodb-enterprise-unstable-server",
            "mongodb-enterprise-unstable-mongos",
            "mongodb-database-tools",
            "mongodb-mongosh",
        ]

        self.assertEqual("server", under_test.get_package_kind(package_names))

    def test_detects_crypt_v1_package_sets(self):
        package_names = [
            "mongodb-enterprise-unstable-crypt-v1",
            "mongodb-database-tools",
            "mongodb-mongosh",
        ]

        self.assertEqual("crypt_v1", under_test.get_package_kind(package_names))

    def test_unknown_package_set_raises(self):
        with self.assertRaises(RuntimeError):
            under_test.get_package_kind(["mongodb-database-tools", "mongodb-mongosh"])

    def test_detects_org_package_edition(self):
        self.assertEqual(
            "org",
            under_test.get_package_edition(
                ["mongodb-org", "mongodb-org-server", "mongodb-database-tools"]
            ),
        )

    def test_detects_enterprise_package_edition(self):
        self.assertEqual(
            "enterprise",
            under_test.get_package_edition(
                ["mongodb-enterprise", "mongodb-enterprise-server", "mongodb-database-tools"]
            ),
        )

    def test_binary_edition_accepts_unstable_expected_edition(self):
        with mock.patch.object(
            under_test,
            "run_and_log",
            return_value=mock.Mock(stdout=b'Build Info: {"modules": ["enterprise"]}'),
        ):
            under_test.test_binary_edition(
                {"package_names": ["mongodb-enterprise-unstable"]}, "enterprise-unstable"
            )

    def test_parses_org_binary_edition(self):
        version_output = 'db version v8.0.0\nBuild Info: {"modules": []}\n'

        self.assertEqual("org", under_test.get_binary_edition(version_output))

    def test_parses_enterprise_binary_edition(self):
        version_output = 'db version v8.0.0\nBuild Info: {"modules": ["enterprise"]}\n'

        self.assertEqual("enterprise", under_test.get_binary_edition(version_output))

    def test_binary_edition_requires_build_info(self):
        with self.assertRaisesRegex(RuntimeError, "build info"):
            under_test.get_binary_edition("db version v8.0.0\n")

    def test_binary_edition_rejects_unknown_modules(self):
        with self.assertRaisesRegex(RuntimeError, "determine edition"):
            under_test.get_binary_edition('Build Info: {"modules": ["atlas"]}')

    def test_binary_edition_rejects_mismatched_package(self):
        with mock.patch.object(
            under_test,
            "run_and_log",
            return_value=mock.Mock(stdout=b'Build Info: {"modules": []}'),
        ):
            with self.assertRaisesRegex(RuntimeError, "does not match"):
                under_test.test_binary_edition(
                    {"package_names": ["mongodb-enterprise"]}, "enterprise"
                )

    def test_parses_readelf_direct_dependencies(self):
        readelf_output = """
 0x0000000000000001 (NEEDED)             Shared library: [libcurl.so.4]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
"""

        self.assertEqual(
            {"libcurl.so.4", "libc.so.6"}, under_test.parse_readelf_dependencies(readelf_output)
        )

    def test_parses_readelf_direct_dependencies_without_architecture_loader(self):
        readelf_output = """
 0x0000000000000001 (NEEDED)             Shared library: [ld-linux-aarch64.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [ld-linux-x86-64.so.2]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
"""

        self.assertEqual({"libc.so.6"}, under_test.parse_readelf_dependencies(readelf_output))

    def test_parses_ldd_dependencies(self):
        ldd_output = """
linux-vdso.so.1 (0x0000ffff)
libcurl.so.4 => /lib/aarch64-linux-gnu/libcurl.so.4 (0x0000ffff)
/lib/ld-linux-aarch64.so.1 (0x0000ffff)
"""

        self.assertEqual(
            {
                "libcurl.so.4": "/lib/aarch64-linux-gnu/libcurl.so.4",
                "ld-linux-aarch64.so.1": "/lib/ld-linux-aarch64.so.1",
            },
            under_test.parse_ldd_dependencies(ldd_output),
        )

    def test_expected_direct_dependencies_do_not_include_architecture_loader(self):
        expected = under_test.get_expected_direct_system_libraries("ubuntu2204", "org")

        self.assertIn("libcrypto.so.3", expected)
        self.assertNotIn("ld-linux-aarch64.so.1", expected)
        self.assertNotIn("ld-linux-x86-64.so.2", expected)

    def test_system_dependency_check_checks_mongod_and_mongos(self):
        test_args = {
            "package_manager": "apt",
            "package_names": ["mongodb-org"],
            "package_kind": "server",
            "platform": "ubuntu2204",
            "os_name": "ubuntu",
            "arch": "x86_64",
        }
        expected = under_test.get_expected_direct_system_libraries("ubuntu2204", "org")
        resolved_paths = {dependency: "/lib/" + dependency for dependency in expected}

        with (
            mock.patch.object(under_test.pathlib.Path, "is_file", return_value=True),
            mock.patch.object(
                under_test,
                "get_binary_system_dependencies",
                return_value=(expected, set(), resolved_paths),
            ) as get_dependencies,
            mock.patch.object(under_test, "get_system_package_names", return_value=["libc6"]),
        ):
            under_test.test_binary_system_dependencies(test_args)

        self.assertEqual(2, get_dependencies.call_count)

    def test_system_dependency_check_rejects_unexpected_direct_dependency(self):
        test_args = {
            "package_manager": "apt",
            "package_names": ["mongodb-org"],
            "package_kind": "server",
            "platform": "ubuntu2204",
            "os_name": "ubuntu",
            "arch": "x86_64",
        }
        expected = under_test.get_expected_direct_system_libraries("ubuntu2204", "org")
        actual = expected | {"libunexpected.so.1"}
        resolved_paths = {dependency: "/lib/" + dependency for dependency in actual}

        with (
            mock.patch.object(under_test.pathlib.Path, "is_file", return_value=True),
            mock.patch.object(
                under_test,
                "get_binary_system_dependencies",
                return_value=(actual, set(), resolved_paths),
            ),
            mock.patch.object(under_test, "get_system_package_names", return_value=[]),
        ):
            with self.assertRaisesRegex(RuntimeError, "unexpected: libunexpected.so.1"):
                under_test.test_binary_system_dependencies(test_args)

    def test_server_package_tests_can_skip_system_dependency_check(self):
        test_args = {"package_kind": "server"}

        with (
            mock.patch.object(under_test, "test_binary_edition"),
            mock.patch.object(under_test, "test_binary_system_dependencies") as check,
            mock.patch.object(under_test, "setup"),
            mock.patch.object(under_test, "install_fake_systemd"),
            mock.patch.object(under_test, "test_start"),
        ):
            under_test.run_server_package_tests(test_args, "org", skip_system_library_check=True)
            check.assert_not_called()

            under_test.run_server_package_tests(test_args, "org")
            check.assert_called_once_with(test_args)

    def test_parses_system_library_check_option(self):
        self.assertEqual(
            (
                "ubuntu2404",
                ["package.tgz"],
                True,
                "server",
            ),
            under_test.parse_package_test_arguments(
                ["--platform", "ubuntu2404", "--skip-system-library-check", "package.tgz"]
            ),
        )

    def test_defaults_to_running_system_library_check(self):
        self.assertEqual(
            ("ubuntu2404", ["package.tgz"], False, "server"),
            under_test.parse_package_test_arguments(["--platform", "ubuntu2404", "package.tgz"]),
        )

    def test_parses_crypt_package_kind(self):
        self.assertEqual(
            ("ubuntu2404", ["package.tgz"], False, "crypt_v1"),
            under_test.parse_package_test_arguments(
                ["--platform", "ubuntu2404", "--package-kind", "crypt_v1", "package.tgz"]
            ),
        )

    def test_invalid_package_kind_is_rejected(self):
        for package_args, expected_error in (
            (
                ["--package-kind"],
                r"Missing value for --package-kind \(expected one of: server, crypt_v1\)",
            ),
            (
                ["--package-kind", "unknown", "package.tgz"],
                "Unsupported package kind: unknown",
            ),
        ):
            with self.subTest(package_args=package_args):
                with self.assertRaisesRegex(ValueError, expected_error):
                    under_test.parse_package_test_arguments(package_args)

    def test_server_test_rejects_crypt_only_packages(self):
        with (
            mock.patch.object(
                sys, "argv", ["test.py", "test.log", "--edition", "enterprise", "package.tgz"]
            ),
            mock.patch.object(under_test, "configure_logging"),
            mock.patch.object(
                under_test, "download_extract_all_packages", return_value=["crypt.rpm"]
            ),
            mock.patch.object(under_test, "run_and_log", return_value=mock.Mock(returncode=0)),
            mock.patch.object(under_test, "run_apt_test"),
            mock.patch.object(
                under_test, "get_test_args", return_value={"package_kind": "crypt_v1"}
            ),
            mock.patch.object(under_test, "test_install_is_complete") as check_install,
            mock.patch.object(under_test, "test_uninstall"),
            mock.patch.object(under_test, "test_uninstall_is_complete"),
        ):
            self.assertEqual(1, under_test.main())
            check_install.assert_not_called()

    def test_crypt_test_accepts_crypt_only_packages(self):
        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "test.py",
                    "test.log",
                    "--edition",
                    "enterprise",
                    "--package-kind",
                    "crypt_v1",
                    "package.tgz",
                ],
            ),
            mock.patch.object(under_test, "configure_logging"),
            mock.patch.object(
                under_test, "download_extract_all_packages", return_value=["crypt.rpm"]
            ),
            mock.patch.object(under_test, "run_and_log", return_value=mock.Mock(returncode=0)),
            mock.patch.object(under_test, "run_apt_test"),
            mock.patch.object(
                under_test, "get_test_args", return_value={"package_kind": "crypt_v1"}
            ),
            mock.patch.object(under_test, "run_server_package_tests") as check_server,
            mock.patch.object(under_test, "test_install_is_complete") as check_install,
            mock.patch.object(under_test, "test_uninstall"),
            mock.patch.object(under_test, "test_uninstall_is_complete"),
        ):
            self.assertEqual(0, under_test.main())
            check_server.assert_not_called()
            check_install.assert_called_once()

    def test_server_required_files_include_mongod_service(self):
        test_args = {
            "package_kind": "server",
            "systemd_units_dir": "/usr/lib/systemd/system",
        }

        self.assertEqual(
            [
                pathlib.Path("/etc/mongod.conf"),
                pathlib.Path("/usr/bin/mongod"),
                pathlib.Path("/usr/bin/mongos"),
                pathlib.Path("/var/log/mongodb/mongod.log"),
                pathlib.Path("/usr/lib/systemd/system/mongod.service"),
            ],
            under_test.get_required_files(test_args),
        )

        self.assertEqual(
            [
                pathlib.Path("/usr/bin/mongod"),
                pathlib.Path("/usr/bin/mongos"),
                pathlib.Path("/usr/lib/systemd/system/mongod.service"),
            ],
            under_test.get_leftover_files(test_args),
        )

    def test_crypt_v1_required_files_follow_libdir(self):
        test_args = {
            "package_kind": "crypt_v1",
            "lib_dir": "/usr/lib64",
        }

        self.assertEqual(
            [
                pathlib.Path("/usr/include/mongo_crypt/v1/mongo_crypt/mongo_crypt.h"),
                pathlib.Path("/usr/lib64/mongo_crypt_v1.so"),
            ],
            under_test.get_required_files(test_args),
        )

        self.assertEqual(
            [
                pathlib.Path("/usr/include/mongo_crypt/v1/mongo_crypt/mongo_crypt.h"),
                pathlib.Path("/usr/lib64/mongo_crypt_v1.so"),
            ],
            under_test.get_leftover_files(test_args),
        )


if __name__ == "__main__":
    unittest.main()
