"""Unit tests for tooling_metrics.py."""
import asyncio
from datetime import datetime
import unittest
from unittest.mock import patch
import mongomock
import pymongo
from buildscripts.metrics.metrics_datatypes import ToolingMetrics
import buildscripts.metrics.tooling_metrics as under_test
from buildscripts.resmoke import entrypoint

# pylint: disable=unused-argument
# pylint: disable=protected-access

TEST_INTERNAL_TOOLING_METRICS_HOSTNAME = 'mongodb://testing:27017'
CURRENT_DATE_TIME = datetime(2022, 10, 4)


async def extended_sleep(arg):
    await asyncio.sleep(2)


@patch("buildscripts.metrics.tooling_metrics.INTERNAL_TOOLING_METRICS_HOSTNAME",
       TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
class TestToolingMetricsCollection(unittest.TestCase):
    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=True)
    def test_on_virtual_workstation(self, mock_is_virtual_workstation):
        under_test.save_tooling_metrics(CURRENT_DATE_TIME)
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=False)
    def test_not_on_virtual_workstation(self, mock_is_virtual_workstation):
        under_test.save_tooling_metrics(CURRENT_DATE_TIME)
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics._save_metrics",
           side_effect=pymongo.errors.WriteError(error="Error Information"))
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=True)
    def test_exception_caught(self, mock_is_virtual_workstation, mock_save_metrics):
        with self.assertLogs('tooling_metrics_collection') as cm:
            under_test.save_tooling_metrics(CURRENT_DATE_TIME)
        assert "Error Information" in cm.output[0]
        assert "Unexpected: Tooling metrics collection is not available" in cm.output[0]
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics._save_metrics", side_effect=extended_sleep)
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=True)
    def test_timeout_caught(self, mock_is_virtual_workstation, mock_save_metrics):
        with self.assertLogs('tooling_metrics_collection') as cm:
            under_test.save_tooling_metrics(CURRENT_DATE_TIME)
        assert "Timeout: Tooling metrics collection is not available" in cm.output[0]
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()


class TestIsVirtualWorkstation(unittest.TestCase):
    @patch("buildscripts.metrics.tooling_metrics._toolchain_exists", return_value=False)
    @patch("buildscripts.metrics.tooling_metrics._git_user_exists", return_value=True)
    def test_no_toolchain_has_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert not under_test._is_virtual_workstation()

    @patch("buildscripts.metrics.tooling_metrics._toolchain_exists", return_value=True)
    @patch("buildscripts.metrics.tooling_metrics._git_user_exists", return_value=True)
    def test_has_toolchain_has_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert under_test._is_virtual_workstation()

    @patch("buildscripts.metrics.tooling_metrics._toolchain_exists", return_value=True)
    @patch("buildscripts.metrics.tooling_metrics._git_user_exists", return_value=False)
    def test_has_toolchain_no_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert not under_test._is_virtual_workstation()

    @patch("buildscripts.metrics.tooling_metrics._toolchain_exists", return_value=False)
    @patch("buildscripts.metrics.tooling_metrics._git_user_exists", return_value=False)
    def test_no_toolchain_no_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert not under_test._is_virtual_workstation()


@patch("buildscripts.metrics.tooling_metrics.INTERNAL_TOOLING_METRICS_HOSTNAME",
       TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
@patch("buildscripts.resmokelib.logging.flush._FLUSH_THREAD", None)
@patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=True)
class TestResmokeMetricsCollection(unittest.TestCase):
    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("sys.argv", ['buildscripts/resmoke.py', 'run', '--suite', 'buildscripts_test'])
    @patch("buildscripts.resmokelib.testing.executor.TestSuiteExecutor._run_tests",
           side_effect=Exception())
    def test_resmoke_metrics_collection_exc(self, mock_executor_run, mock_is_virtual_workstation):
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()
        with self.assertRaises(SystemExit):
            entrypoint()
        assert client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    def test_resmoke_metrics_collection(self, mock_is_virtual_workstation):
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()
        entrypoint()
        assert client.metrics.tooling_metrics.find_one()
