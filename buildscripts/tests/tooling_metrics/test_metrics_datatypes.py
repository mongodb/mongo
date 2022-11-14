"""Unit tests for metrics_datatypes.py."""
from datetime import datetime
import unittest
from unittest.mock import patch

from mock import MagicMock

import buildscripts.metrics.metrics_datatypes as under_test

# pylint: disable=unused-argument


@patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_artifact_dir",
       return_value='/test')
class TestBuildInfo(unittest.TestCase):
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_env_vars_dict",
           return_value={'env': 'env'})
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_options_dict",
           return_value={'opt': 'opt'})
    def test_build_info_valid(self, mock_env, mock_options, mock_artifact_dir):
        build_info = under_test.BuildInfo.get_scons_build_info(datetime.utcnow(), MagicMock(),
                                                               MagicMock(), MagicMock(),
                                                               MagicMock())
        assert not build_info.is_malformed()

    def test_build_info_malformed(self, mock_artifact_dir):
        build_info = under_test.BuildInfo.get_scons_build_info(datetime.utcnow(), MagicMock(),
                                                               MagicMock(), MagicMock(),
                                                               MagicMock())
        assert build_info.is_malformed()


class TestExitInfo(unittest.TestCase):
    @patch("sys.exc_info", return_value=(None, None, None))
    def test_resmoke_no_exc_info(self, mock_exc_info):
        exit_info = under_test.ExitInfo.get_resmoke_exit_info()
        assert not exit_info.is_malformed()

    @patch("sys.exc_info", return_value=(None, ValueError(), None))
    def test_resmoke_with_exc_info(self, mock_exc_info):
        exit_info = under_test.ExitInfo.get_resmoke_exit_info()
        assert not exit_info.is_malformed()

    def test_scons_exit_info_valid(self):
        exit_info = under_test.ExitInfo.get_scons_exit_info(0)
        assert not exit_info.is_malformed()

    def test_scons_exit_info_malformed(self):
        exit_info = under_test.ExitInfo.get_scons_exit_info('string')
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
    def test_resmoke_tooling_metrics_with_exc(self, mock_gethostname, mock_get_memory):
        tooling_metrics = under_test.ToolingMetrics.get_resmoke_metrics(datetime.utcnow())
        assert tooling_metrics.is_malformed()

    def test_resmoke_tooling_metrics_no_exc(self, mock_get_memory):
        tooling_metrics = under_test.ToolingMetrics.get_resmoke_metrics(datetime.utcnow())
        assert not tooling_metrics.is_malformed()

    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_artifact_dir",
           return_value='/test')
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_env_vars_dict",
           return_value={'env': 'env'})
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_options_dict",
           return_value={'opt': 'opt'})
    def test_scons_tooling_metrics_valid(self, mock_options, mock_env, mock_artifact_dir,
                                         mock_get_memory):
        parser = MagicMock()
        parser.parse_args = MagicMock(return_value={"opt1": "val1"})
        tooling_metrics = under_test.ToolingMetrics.get_scons_metrics(
            datetime.utcnow(), {'env': 'env'}, {'opts': 'opts'}, parser, ['test1', 'test2'], 0)
        assert not tooling_metrics.is_malformed()

    def test_scons_tooling_metrics_malformed(self, mock_get_memory):
        tooling_metrics = under_test.ToolingMetrics.get_scons_metrics(
            datetime.utcnow(), {'env': 'env'}, {'opts': 'opts'}, None, [], 0)
        assert tooling_metrics.is_malformed()
