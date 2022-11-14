from datetime import datetime
import os
import sys
import unittest
from unittest.mock import patch
import mongomock
import pymongo

import buildscripts.metrics.resmoke_tooling_metrics as under_test
from buildscripts.resmoke import entrypoint as resmoke_entrypoint

TEST_INTERNAL_TOOLING_METRICS_HOSTNAME = 'mongodb://testing:27017'
CURRENT_DATE_TIME = datetime(2022, 10, 4)

# pylint: disable=unused-argument

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


@patch("buildscripts.metrics.tooling_metrics_utils.INTERNAL_TOOLING_METRICS_HOSTNAME",
       TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
@patch("buildscripts.resmokelib.logging.flush._FLUSH_THREAD", None)
class TestResmokeMetricsCollection(unittest.TestCase):
    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.resmoke_tooling_metrics.should_collect_metrics", return_value=True)
    @patch("sys.argv", ['buildscripts/resmoke.py', 'run', '--suite', 'buildscripts_test'])
    @patch("buildscripts.resmokelib.testing.executor.TestSuiteExecutor._run_tests",
           side_effect=Exception())
    def test_resmoke_metrics_collection_exc(self, mock_executor_run, mock_should_collect_metrics):
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()
        with self.assertRaises(SystemExit):
            resmoke_entrypoint()
        assert client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.resmoke_tooling_metrics.should_collect_metrics", return_value=True)
    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    def test_resmoke_metrics_collection(self, mock_should_collect_metrics):
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()
        resmoke_entrypoint()
        assert client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    @patch("buildscripts.metrics.resmoke_tooling_metrics.should_collect_metrics",
           return_value=False)
    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    def test_no_resmoke_metrics_collection(self, mock_should_collect_metrics):
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()
        resmoke_entrypoint()
        assert not client.metrics.tooling_metrics.find_one()
