"""Unit tests for metrics_datatypes.py."""
from datetime import datetime
import unittest
from unittest.mock import patch

import buildscripts.metrics.metrics_datatypes as under_test

# pylint: disable=unused-argument


class TestExitInfo(unittest.TestCase):
    @patch("sys.exc_info", return_value=(None, None, None))
    def test_no_exc_info(self, mock_exc_info):
        exit_info = under_test.ExitInfo.get_exit_info()
        assert exit_info.is_malformed()

    @patch("sys.exc_info", return_value=(None, ValueError(), None))
    def test_with_exc_info(self, mock_exc_info):
        exit_info = under_test.ExitInfo.get_exit_info()
        assert exit_info.is_malformed()


class TestHostInfo(unittest.TestCase):
    @patch("buildscripts.metrics.metrics_datatypes.HostInfo._get_memory", side_effect=Exception())
    def test_host_info_with_exc(self, mock_get_memory):
        host_info = under_test.HostInfo.get_host_info()
        assert host_info.is_malformed()

    # Mock this so that it passes when running the 'buildscripts_test' suite on Windows
    @patch("buildscripts.metrics.metrics_datatypes.HostInfo._get_memory", return_value=30)
    def test_host_info_no_exc(self, mock_get_memory):
        host_info = under_test.HostInfo.get_host_info()
        assert not host_info.is_malformed()


class TestGitInfo(unittest.TestCase):
    @patch("git.Repo", side_effect=Exception())
    def test_git_info_with_exc(self, mock_repo):
        git_info = under_test.GitInfo.get_git_info('.')
        assert git_info.is_malformed()

    def test_git_info_no_exc(self):
        git_info = under_test.GitInfo.get_git_info('.')
        assert not git_info.is_malformed()

    @patch("git.refs.symbolic.SymbolicReference.is_detached", True)
    def test_git_info_detached_head(self):
        git_info = under_test.GitInfo.get_git_info('.')
        assert not git_info.is_malformed()


# Mock this so that it passes when running the 'buildscripts_test' suite on Windows
@patch("buildscripts.metrics.metrics_datatypes.HostInfo._get_memory", return_value=30)
class TestToolingMetrics(unittest.TestCase):
    @patch("socket.gethostname", side_effect=Exception())
    def test_tooling_metrics_with_exc(self, mock_gethostname, mock_get_memory):
        tooling_metrics = under_test.ToolingMetrics.get_tooling_metrics(datetime.utcnow())
        assert tooling_metrics.is_malformed()

    def test_tooling_metrics_no_exc(self, mock_get_memory):
        tooling_metrics = under_test.ToolingMetrics.get_tooling_metrics(datetime.utcnow())
        assert not tooling_metrics.is_malformed()
