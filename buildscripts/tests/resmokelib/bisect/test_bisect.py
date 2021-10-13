"""Unit tests for buildscripts/resmokelib/bisect."""
# pylint: disable=missing-docstring
# pylint: disable=no-self-use
# pylint: disable=protected-access
import unittest
from os.path import exists

import buildscripts.resmokelib.bisect as under_test


def mock_run_test_script(arg):
    return "success" in arg


class TestBisectBase(unittest.TestCase):
    def setUp(self):
        mock_bisect = under_test.Bisect(
            branch="mongodb-mongo-master",
            lookback=10,
            evergreen_config=None,
            variant="enterprise-macos",
            script="/file/path/to/test/shell/script",
            debug=None,
            python_installation="python",
        )
        mock_bisect._test_version_with_script = mock_run_test_script
        self.bisect = mock_bisect


class TestBisect(TestBisectBase):
    def test_bisect_standard(self):
        versions = ["success1", "success2", "success3", "fail1", "fail2", "fail3"]
        self.assertEqual("success3", self.bisect.bisect(versions))

    def test_bisect_empty(self):
        versions = []
        self.assertEqual(None, self.bisect.bisect(versions))

    def test_bisect_all_success(self):
        versions = ["success1", "success2", "success3", "success4"]
        self.assertEqual("success4", self.bisect.bisect(versions))

    def test_bisect_all_fail(self):
        versions = ["fail1", "fail2", "fail3"]
        self.assertEqual(None, self.bisect.bisect(versions))


class TestFilePaths(unittest.TestCase):
    def test_setup_test_env_filepath(self):
        assert exists(under_test.SETUP_TEST_ENV_SH)

    def test_teardown_test_env_filepath(self):
        assert exists(under_test.TEARDOWN_TEST_ENV_SH)

    def test_run_user_script_filepath(self):
        assert exists(under_test.RUN_USER_SCRIPT_SH)

    def test_resmoke_filepath(self):
        assert exists(under_test.RESMOKE_FILEPATH)
