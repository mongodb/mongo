import pathlib
import sys
import unittest
from unittest import mock

from buildscripts import package_test_internal as under_test


class PackageTestInternalHelpersTest(unittest.TestCase):
    def test_package_kind_defaults_to_server(self):
        self.assertEqual(
            (["package.tgz"], "server"),
            under_test.parse_package_test_arguments(["package.tgz"]),
        )

    def test_parses_crypt_package_kind(self):
        self.assertEqual(
            (["package.tgz"], "crypt_v1"),
            under_test.parse_package_test_arguments(["--package-kind", "crypt_v1", "package.tgz"]),
        )

    def test_invalid_package_kind_is_rejected(self):
        for package_args, error in (
            (["--package-kind"], "Missing value for --package-kind"),
            (["--package-kind", "unknown", "package.tgz"], "Unsupported package kind: unknown"),
        ):
            with self.subTest(package_args=package_args):
                with self.assertRaisesRegex(ValueError, error):
                    under_test.parse_package_test_arguments(package_args)

    def test_server_test_rejects_crypt_only_packages(self):
        with (
            mock.patch.object(sys, "argv", ["test.py", "test.log", "package.tgz"]),
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
                sys, "argv", ["test.py", "test.log", "--package-kind", "crypt_v1", "package.tgz"]
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
            mock.patch.object(under_test, "setup") as setup_server,
            mock.patch.object(under_test, "test_install_is_complete") as check_install,
            mock.patch.object(under_test, "test_uninstall"),
            mock.patch.object(under_test, "test_uninstall_is_complete"),
        ):
            self.assertEqual(0, under_test.main())
            setup_server.assert_not_called()
            check_install.assert_called_once()

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
