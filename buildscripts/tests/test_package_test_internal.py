import pathlib
import unittest
from unittest import mock

from buildscripts.package_test import package_test_internal as under_test


class PackageTestInternalHelpersTest(unittest.TestCase):
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

    def test_server_required_files_include_mongod_service(self):
        test_args = {
            "package_kind": "server",
            "systemd_units_dir": "/usr/lib/systemd/system",
        }

        self.assertEqual(
            [
                pathlib.Path("/etc/mongod.conf"),
                pathlib.Path("/usr/bin/mongod"),
                pathlib.Path("/var/log/mongodb/mongod.log"),
                pathlib.Path("/usr/lib/systemd/system/mongod.service"),
            ],
            under_test.get_required_files(test_args),
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
