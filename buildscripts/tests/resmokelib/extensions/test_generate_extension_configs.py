"""Unit tests for the buildscripts.resmokelib.extensions.generate_extension_configs module."""

import logging
import os
import shutil
import stat
import sys
import tempfile
import unittest

from buildscripts.resmokelib.extensions.generate_extension_configs import (
    generate_extension_configs,
    get_conf_out_dir,
)


@unittest.skipIf(
    sys.platform != "linux",
    "POSIX permission bits are only meaningful on Linux; the permission check under test "
    "(verifyConfigPathPermissions) is itself only enforced on Linux.",
)
class TestGenerateExtensionConfigs(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmpdir, ignore_errors=True)

        # get_conf_out_dir() resolves its output directory from TMPDIR, so point it at a
        # directory we control and can inspect.
        self.tmpdir_patcher_env = os.environ.copy()
        os.environ["TMPDIR"] = self.tmpdir
        self.addCleanup(self._restore_environ)

        self.logger = logging.getLogger("test_generate_extension_configs")
        self.logger.addHandler(logging.NullHandler())

    def _restore_environ(self):
        os.environ.clear()
        os.environ.update(self.tmpdir_patcher_env)

    def test_conf_dir_is_created_with_restricted_permissions(self):
        generate_extension_configs(
            so_files=["/path/to/libfoo_mongo_extension.so"],
            with_suffix="suffix",
            logger=self.logger,
            manual_options="{}",
        )

        conf_out_dir = get_conf_out_dir()
        mode = stat.S_IMODE(os.stat(conf_out_dir).st_mode)
        self.assertEqual(mode, 0o700)

    def test_conf_dir_permissions_are_fixed_when_dir_already_exists(self):
        # Simulate a conf dir left over from a previous run with a permissive umask: makedirs()
        # only applies `mode` when it creates the directory, so a pre-existing, overly-permissive
        # directory would otherwise be left alone.
        conf_out_dir = get_conf_out_dir()
        os.makedirs(conf_out_dir, mode=0o777, exist_ok=True)
        os.chmod(conf_out_dir, 0o777)

        generate_extension_configs(
            so_files=["/path/to/libfoo_mongo_extension.so"],
            with_suffix="suffix",
            logger=self.logger,
            manual_options="{}",
        )

        mode = stat.S_IMODE(os.stat(conf_out_dir).st_mode)
        self.assertEqual(mode, 0o700)

    def test_conf_file_is_created_with_restricted_permissions(self):
        extension_names = generate_extension_configs(
            so_files=["/path/to/libfoo_mongo_extension.so"],
            with_suffix="suffix",
            logger=self.logger,
            manual_options="{}",
        )

        conf_out_dir = get_conf_out_dir()
        conf_file_path = os.path.join(conf_out_dir, f"{extension_names[0]}.conf")
        mode = stat.S_IMODE(os.stat(conf_file_path).st_mode)
        self.assertEqual(mode, 0o600)


if __name__ == "__main__":
    unittest.main()
