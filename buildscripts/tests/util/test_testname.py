"""Unit test for the util.testname module."""

import unittest

import buildscripts.util.testname as testname_utils


class IsResmokeHookTest(unittest.TestCase):
    def test_is_a_test_hook_true(self):
        self.assertTrue(testname_utils.is_resmoke_hook("testname:hook"))

    def test_is_a_test_hook_false(self):
        self.assertFalse(testname_utils.is_resmoke_hook("testnameWithoutHook"))


class SplitTestHookNameTest(unittest.TestCase):
    def test_split_test_hook_returns_test_name_and_hook(self):
        (test_name, hook_name) = testname_utils.split_test_hook_name("test:hook")

        self.assertEqual(test_name, "test")
        self.assertEqual(hook_name, "hook")

    def test_split_test_hook_name_should_throw_exception(self):
        with self.assertRaises(AssertionError):
            testname_utils.split_test_hook_name("test")


class GetShortNameFromTestFileTest(unittest.TestCase):
    def test_only_base_name_is_returned(self):
        hook_name = testname_utils.get_short_name_from_test_file("path/to/test_file")

        self.assertEqual(hook_name, "test_file")

    def test_extension_is_not_returned(self):
        hook_name = testname_utils.get_short_name_from_test_file("test_file.sh")

        self.assertEqual(hook_name, "test_file")

    def test_full_path_and_extension_are_stripped(self):
        hook_name = testname_utils.get_short_name_from_test_file("path/to/test_file.sh")

        self.assertEqual(hook_name, "test_file")


class NormalizeTestFileTest(unittest.TestCase):
    def test_windows_file_is_normalized(self):
        windows_file = "test\\found\\under\\windows.exe"
        self.assertEqual(
            testname_utils.normalize_test_file(windows_file), "test/found/under/windows")

    def test_windows_file_with_non_exe_ext(self):
        windows_file = "test\\found\\under\\windows.sh"
        self.assertEqual(
            testname_utils.normalize_test_file(windows_file), "test/found/under/windows.sh")

    def test_unix_files_are_not_changed(self):
        unix_file = "test/found/under/unix"
        self.assertEqual(testname_utils.normalize_test_file(unix_file), unix_file)


class DenormalizeTestFileTest(unittest.TestCase):
    def test_windows_file_is_denormalized(self):
        windows_file = "test\\found\\under\\windows.exe"
        expected = ["test/found/under/windows", windows_file]
        self.assertEqual(expected, testname_utils.denormalize_test_file(windows_file))

    def test_windows_file_with_non_exe_ext(self):
        windows_file = "test\\found\\under\\windows.sh"
        expected = ["test/found/under/windows.sh", windows_file]
        self.assertEqual(expected, testname_utils.denormalize_test_file(windows_file))

    def test_unix_file_is_denormalized(self):
        unix_file = "test/found/under/unix"
        expected = [unix_file, "test\\found\\under\\unix.exe"]
        self.assertEqual(expected, testname_utils.denormalize_test_file(unix_file))

    def test_unix_file_with_ext(self):
        unix_file = "test/found/under/unix.sh"
        expected = [unix_file, "test\\found\\under\\unix.sh"]
        self.assertEqual(expected, testname_utils.denormalize_test_file(unix_file))
