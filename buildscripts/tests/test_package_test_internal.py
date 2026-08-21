import gzip
import pathlib
import tempfile
import unittest
from unittest import mock

from buildscripts.package_test import package_test_internal as under_test


class PackageTestInternalHelpersTest(unittest.TestCase):
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
            ),
            under_test.parse_package_test_arguments(
                ["--platform", "ubuntu2404", "--skip-system-library-check", "package.tgz"]
            ),
        )

    def test_defaults_to_running_system_library_check(self):
        self.assertEqual(
            ("ubuntu2404", ["package.tgz"], False),
            under_test.parse_package_test_arguments(["--platform", "ubuntu2404", "package.tgz"]),
        )

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
