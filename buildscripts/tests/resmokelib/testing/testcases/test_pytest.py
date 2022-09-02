"""Unit tests for the buildscripts.resmokelib.testing.testcases.pytest module."""
import logging
import sys
import unittest

from buildscripts.resmokelib.testing.testcases import pytest

_IS_WINDOWS = sys.platform == "win32"

# pylint: disable=protected-access


def get_filename(filename):
    if _IS_WINDOWS:
        return filename.replace("/", "\\")
    return filename


class TestPyTestCase(unittest.TestCase):
    def test__make_process(self):
        logger = logging.getLogger("pytest")
        filename = "myfile.py"
        pytest_case = pytest.PyTestCase(logger, filename)
        self.assertEqual(pytest_case.test_name, filename)
        self.assertEqual(pytest_case.logger, logger)
        proc = pytest_case._make_process()
        self.assertIn(" -m unittest", proc.as_command())
        self.assertIn(pytest_case.test_module_name, proc.as_command())
        self.assertEqual(proc.logger, logger)

    def test__make_process_dir(self):
        logger = logging.getLogger("pytest")
        filename = "dir1/dir2/myfile.py"
        pytest_case = pytest.PyTestCase(logger, filename)
        self.assertEqual(pytest_case.test_name, filename)
        self.assertEqual(pytest_case.logger, logger)
        proc = pytest_case._make_process()
        self.assertIn(" -m unittest", proc.as_command())
        self.assertIn(pytest_case.test_module_name, proc.as_command())
        self.assertEqual(proc.logger, logger)

    def test__make_process_windows_file(self):
        logger = logging.getLogger("pytest")
        filename = "dir1\\dir2\\myfile.py"
        pytest_case = pytest.PyTestCase(logger, filename)
        self.assertEqual(pytest_case.test_name, filename)
        self.assertEqual(pytest_case.logger, logger)
        proc = pytest_case._make_process()
        self.assertIn(" -m unittest", proc.as_command())
        self.assertIn(pytest_case.test_module_name, proc.as_command())
        self.assertEqual(proc.logger, logger)

    def test_test_module_name(self):
        logger = logging.getLogger("pytest")
        filename = get_filename("dir1/dir2/dir3/myfile.py")
        pytest_case = pytest.PyTestCase(logger, filename)
        self.assertEqual(pytest_case.test_module_name, "dir1.dir2.dir3.myfile")

    def test_test_module_name_absolute_dir(self):
        logger = logging.getLogger("pytest")
        filename = get_filename("/dir1/dir2/dir3/myfile.py")
        pytest_case = pytest.PyTestCase(logger, filename)
        self.assertEqual(pytest_case.test_module_name, ".dir1.dir2.dir3.myfile")

    def test_test_module_name_no_dir(self):
        logger = logging.getLogger("pytest")
        filename = "myfile.py"
        pytest_case = pytest.PyTestCase(logger, filename)
        self.assertEqual(pytest_case.test_module_name, "myfile")

    def test__make_process_with_space(self):
        logger = logging.getLogger("pytest")
        filename = "this file name has space.py:q:"
        pytest_case = pytest.PyTestCase(logger, filename)
        self.assertEqual(pytest_case.test_name, filename)
        self.assertEqual(pytest_case.logger, logger)
        proc = pytest_case._make_process()
        self.assertIn(" -m unittest", proc.as_command())
        self.assertIn(pytest_case.test_module_name, proc.as_command())
        self.assertEqual(proc.logger, logger)
