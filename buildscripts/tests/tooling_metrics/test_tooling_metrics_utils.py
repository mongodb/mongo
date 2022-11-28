"""Unit tests for tooling_metrics.py."""
from datetime import datetime
import os
import sys
import unittest
from unittest.mock import mock_open, patch
from mock import MagicMock
import mongomock
import pymongo
from buildscripts.metrics.metrics_datatypes import ResmokeToolingMetrics, SConsToolingMetrics
import buildscripts.metrics.tooling_metrics_utils as under_test

# pylint: disable=unused-argument
# pylint: disable=protected-access

TEST_INTERNAL_TOOLING_METRICS_HOSTNAME = 'mongodb://testing:27017'
RESMOKE_METRICS_ARGS = {
    "utc_starttime": datetime(2022, 10, 4),
    "exit_hook": MagicMock(exit_code=0),
}

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


@patch("atexit.register")
class TestRegisterMetricsCollectionAtExit(unittest.TestCase):
    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=True)
    def test_register_metrics_collection(self, mock_should_collect_metrics, mock_atexit):
        under_test.register_metrics_collection_atexit(ResmokeToolingMetrics.generate_metrics,
                                                      RESMOKE_METRICS_ARGS)
        atexit_functions = [call[0][0].__name__ for call in mock_atexit.call_args_list]
        assert "_save_metrics" in atexit_functions

    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=False)
    def test_no_register_metrics_collection(self, mock_should_collect_metrics, mock_atexit):
        under_test.register_metrics_collection_atexit(ResmokeToolingMetrics.generate_metrics,
                                                      RESMOKE_METRICS_ARGS)
        atexit_functions = [call[0][0].__name__ for call in mock_atexit.call_args_list]
        assert "_save_metrics" not in atexit_functions


@patch("buildscripts.metrics.tooling_metrics_utils.INTERNAL_TOOLING_METRICS_HOSTNAME",
       TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
class TestSaveToolingMetrics(unittest.TestCase):
    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    def test_save_resmoke_metrics(self):
        under_test._save_metrics(ResmokeToolingMetrics.generate_metrics, RESMOKE_METRICS_ARGS)
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics_utils._get_internal_tooling_metrics_client",
           side_effect=pymongo.errors.ServerSelectionTimeoutError(message="Error Information"))
    def test_save_metrics_with_exc(self, mock_save_metrics):
        with self.assertLogs('tooling_metrics') as cm:
            under_test._save_metrics(ResmokeToolingMetrics.generate_metrics, RESMOKE_METRICS_ARGS)
        assert "Error Information" in cm.output[0]
        assert "Internal Metrics Collection Failed" in cm.output[0]
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()


class TestIsVirtualWorkstation(unittest.TestCase):
    @patch("builtins.open", mock_open(read_data="ubuntu1804-workstation"))
    def test_is_virtual_workstation(self):
        assert under_test._is_virtual_workstation() is True

    @patch("builtins.open", mock_open(read_data="test"))
    def test_is_not_virtual_workstation(self):
        assert under_test._is_virtual_workstation() is False


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
        assert under_test._should_collect_metrics()

    @patch("buildscripts.metrics.tooling_metrics_utils._is_virtual_workstation", return_value=True)
    @patch("buildscripts.metrics.tooling_metrics_utils._has_metrics_opt_out", return_value=True)
    def test_no_collect_metrics_opt_out(self, mock_opt_out, mock_is_virtual_env):
        assert not under_test._should_collect_metrics()
