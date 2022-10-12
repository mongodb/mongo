"""Unit tests for tooling_metrics.py."""
import asyncio
from datetime import datetime
import socket
import unittest
from unittest.mock import patch
import mongomock
import pymongo
import buildscripts.metrics.tooling_metrics as under_test

# pylint: disable=unused-argument
# pylint: disable=protected-access

TEST_INTERNAL_TOOLING_METRICS_HOSTNAME = 'mongodb://testing:27017'
TEST_TOOLING_METRIC_DATA = {
    "command": "buildscripts/resmoke.py test-command --test-option=test-value",
    "utc_starttime": datetime(2022, 10, 4, 21, 52, 17),
    "utc_endtime": datetime(2022, 10, 4, 21, 53, 16),
}
TEST_TOOLING_METRICS = under_test.ToolingMetrics(**TEST_TOOLING_METRIC_DATA)


async def extended_sleep(arg):
    await asyncio.sleep(2)


class TestToolingMetricsCollection(unittest.TestCase):
    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics.INTERNAL_TOOLING_METRICS_HOSTNAME",
           TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=True)
    def test_on_virtual_workstation(self, mock_is_virtual_workstation):
        under_test.save_tooling_metrics(TEST_TOOLING_METRICS)
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        inserted_doc = client.metrics.tooling_metrics.find_one()
        del inserted_doc['_id']
        expected_doc = {"ip_address": socket.gethostbyname(socket.gethostname())}
        expected_doc.update(inserted_doc)
        assert inserted_doc == expected_doc

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics.INTERNAL_TOOLING_METRICS_HOSTNAME",
           TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=False)
    def test_not_on_virtual_workstation(self, mock_is_virtual_workstation):
        under_test.save_tooling_metrics(TEST_TOOLING_METRICS)
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics.INTERNAL_TOOLING_METRICS_HOSTNAME",
           TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
    @patch("buildscripts.metrics.tooling_metrics._save_metrics",
           side_effect=pymongo.errors.WriteError(error="Error Information"))
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=True)
    def test_exception_caught(self, mock_is_virtual_workstation, mock_save_metrics):
        with self.assertLogs('tooling_metrics_collection') as cm:
            under_test.save_tooling_metrics(TEST_TOOLING_METRICS)
        assert "Error Information" in cm.output[0]
        assert "Unexpected: Could not save tooling metrics data to MongoDB Atlas Cluster." in cm.output[
            0]
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.tooling_metrics.INTERNAL_TOOLING_METRICS_HOSTNAME",
           TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
    @patch("buildscripts.metrics.tooling_metrics._save_metrics", side_effect=extended_sleep)
    @patch("buildscripts.metrics.tooling_metrics._is_virtual_workstation", return_value=True)
    def test_timeout_caught(self, mock_is_virtual_workstation, mock_save_metrics):
        with self.assertLogs('tooling_metrics_collection') as cm:
            under_test.save_tooling_metrics(TEST_TOOLING_METRICS)
        assert "Timeout: Could not save tooling metrics data to MongoDB Atlas Cluster." in cm.output[
            0]
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
