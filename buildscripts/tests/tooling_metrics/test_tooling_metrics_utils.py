"""Unit tests for tooling_metrics.py."""
import asyncio
from datetime import datetime
import os
import sys
import unittest
from unittest.mock import patch
import mongomock
import pymongo
from buildscripts.metrics.metrics_datatypes import ToolingMetrics
import buildscripts.metrics.tooling_metrics_utils as under_test

# pylint: disable=unused-argument
# pylint: disable=protected-access

TEST_INTERNAL_TOOLING_METRICS_HOSTNAME = 'mongodb://testing:27017'
CURRENT_DATE_TIME = datetime(2022, 10, 4)

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


@patch("buildscripts.metrics.tooling_metrics_utils.INTERNAL_TOOLING_METRICS_HOSTNAME",
       TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
class TestSaveToolingMetrics(unittest.TestCase):
    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    def test_on_virtual_workstation(self):
        under_test.save_tooling_metrics(ToolingMetrics.get_resmoke_metrics(CURRENT_DATE_TIME))
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics_utils._save_metrics",
           side_effect=pymongo.errors.WriteError(error="Error Information"))
    def test_exception_caught(self, mock_save_metrics):
        with self.assertLogs('tooling_metrics_utils') as cm:
            under_test.save_tooling_metrics(ToolingMetrics.get_resmoke_metrics(CURRENT_DATE_TIME))
        assert "Error Information" in cm.output[0]
        assert "Unexpected: Tooling metrics collection is not available" in cm.output[0]
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()


class TestIsVirtualWorkstation(unittest.TestCase):
    @patch("buildscripts.metrics.tooling_metrics_utils._toolchain_exists", return_value=False)
    @patch("buildscripts.metrics.tooling_metrics_utils._git_user_exists", return_value=True)
    def test_no_toolchain_has_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert not under_test._is_virtual_workstation()

    @patch("buildscripts.metrics.tooling_metrics_utils._toolchain_exists", return_value=True)
    @patch("buildscripts.metrics.tooling_metrics_utils._git_user_exists", return_value=True)
    def test_has_toolchain_has_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert under_test._is_virtual_workstation()

    @patch("buildscripts.metrics.tooling_metrics_utils._toolchain_exists", return_value=True)
    @patch("buildscripts.metrics.tooling_metrics_utils._git_user_exists", return_value=False)
    def test_has_toolchain_no_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert not under_test._is_virtual_workstation()

    @patch("buildscripts.metrics.tooling_metrics_utils._toolchain_exists", return_value=False)
    @patch("buildscripts.metrics.tooling_metrics_utils._git_user_exists", return_value=False)
    def test_no_toolchain_no_email(self, mock_git_user_exists, mock_toolchain_exists):
        assert not under_test._is_virtual_workstation()


class TestHasMetricsOptOut(unittest.TestCase):
    @patch("os.environ.get", return_value='1')
    def test_opt_out(self, mock_environ_get):
        assert under_test._has_metrics_opt_out()

    @patch("os.environ.get", return_value=None)
    def test_no_opt_out(self, mock_environ_get):
        assert not under_test._has_metrics_opt_out()


class TestShouldCollectMetrics(unittest.TestCase):
    @patch("buildscripts.metrics.tooling_metrics_utils._is_virtual_workstation", return_value=True)
    @patch("buildscripts.metrics.tooling_metrics_utils._has_metrics_opt_out", return_value=False)
    def test_should_collect_metrics(self, mock_opt_out, mock_is_virtual_env):
        assert under_test.should_collect_metrics()

    @patch("buildscripts.metrics.tooling_metrics_utils._is_virtual_workstation", return_value=True)
    @patch("buildscripts.metrics.tooling_metrics_utils._has_metrics_opt_out", return_value=True)
    def test_no_collect_metrics_opt_out(self, mock_opt_out, mock_is_virtual_env):
        assert not under_test.should_collect_metrics()
