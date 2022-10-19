"""Unit tests for metrics_datatypes.py."""
from datetime import datetime
import unittest
from unittest.mock import patch

import buildscripts.metrics.metrics_datatypes as under_test

# pylint: disable=unused-argument


class TestExitInfo(unittest.TestCase):
    @patch("sys.exc_info", return_value=(None, None, None))
    def test_no_exc_info(self, mock_exc_info):
        exit_info = under_test.ExitInfo()
        assert exit_info

    @patch("sys.exc_info", return_value=(None, ValueError(), None))
    def test_with_exc_info(self, mock_exc_info):
        exit_info = under_test.ExitInfo()
        assert exit_info


class TestHostInfo(unittest.TestCase):
    @patch("buildscripts.metrics.metrics_datatypes.HostInfo._get_memory", side_effect=Exception())
    def test_host_info_with_exc(self, mock_get_memory):
        host_info = under_test.HostInfo()
        assert host_info

    def test_host_info_no_exc(self):
        host_info = under_test.HostInfo()
        assert host_info


class TestGitInfo(unittest.TestCase):
    @patch("git.Repo", side_effect=Exception())
    def test_git_info_with_exc(self, mock_repo):
        git_info = under_test.GitInfo('.')
        assert git_info

    def test_git_info_no_exc(self):
        git_info = under_test.GitInfo('.')
        assert git_info


class TestToolingMetrics(unittest.TestCase):
    @patch("socket.gethostname", side_effect=Exception())
    def test_tooling_metrics_with_exc(self, mock_gethostname):
        tooling_metrics = under_test.ToolingMetrics(datetime.utcnow())
        assert tooling_metrics

    def test_tooling_metrics_no_exc(self):
        tooling_metrics = under_test.ToolingMetrics(datetime.utcnow())
        assert tooling_metrics
