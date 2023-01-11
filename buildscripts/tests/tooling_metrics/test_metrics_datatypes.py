"""Unit tests for metrics_datatypes.py."""
from datetime import datetime
import os
import sys
import unittest
from unittest.mock import patch

from mock import MagicMock

import buildscripts.metrics.metrics_datatypes as under_test

# pylint: disable=unused-argument

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()

MOCK_EXIT_HOOK = MagicMock(exit_code=0)


@patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_artifact_dir",
       return_value='/test')
class TestBuildInfo(unittest.TestCase):
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_env_vars_dict",
           return_value={'env': 'env'})
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_options_dict",
           return_value={'opt': 'opt'})
    def test_build_info_valid(self, mock_env, mock_options, mock_artifact_dir):
        build_info = under_test.BuildInfo.generate_metrics(datetime.utcnow(), MagicMock(),
                                                           MagicMock(), MagicMock(), MagicMock())
        assert not build_info.is_malformed()

    def test_build_info_malformed(self, mock_artifact_dir):
        build_info = under_test.BuildInfo.generate_metrics(datetime.utcnow(), MagicMock(),
                                                           MagicMock(), MagicMock(), MagicMock())
        assert build_info.is_malformed()


class TestHostInfo(unittest.TestCase):
    @patch("buildscripts.metrics.metrics_datatypes.HostInfo._get_memory", side_effect=Exception())
    def test_host_info_with_exc(self, mock_get_memory):
        host_info = under_test.HostInfo.generate_metrics()
        assert host_info.is_malformed()

    # Mock this so that it passes when running the 'buildscripts_test' suite on Windows
    @patch("buildscripts.metrics.metrics_datatypes.HostInfo._get_memory", return_value=30)
    def test_host_info_no_exc(self, mock_get_memory):
        host_info = under_test.HostInfo.generate_metrics()
        assert not host_info.is_malformed()


class TestGitInfo(unittest.TestCase):
    @patch("git.Repo", side_effect=Exception())
    def test_git_info_with_exc(self, mock_repo):
        git_info = under_test.GitInfo.generate_metrics('.')
        assert git_info.is_malformed()

    def test_git_info_no_exc(self):
        git_info = under_test.GitInfo.generate_metrics('.')
        assert not git_info.is_malformed()

    @patch("git.refs.symbolic.SymbolicReference.is_detached", True)
    def test_git_info_detached_head(self):
        git_info = under_test.GitInfo.generate_metrics('.')
        assert not git_info.is_malformed()


class TestResmokeToolingMetrics(unittest.TestCase):
    @patch("socket.gethostname", side_effect=Exception())
    def test_resmoke_tooling_metrics_valid(self, mock_gethostname):
        tooling_metrics = under_test.ResmokeToolingMetrics.generate_metrics(
            datetime.utcnow(),
            MOCK_EXIT_HOOK,
        )
        assert tooling_metrics.is_malformed()

    def test_resmoke_tooling_metrics_malformed(self):
        tooling_metrics = under_test.ResmokeToolingMetrics.generate_metrics(
            datetime.utcnow(),
            MOCK_EXIT_HOOK,
        )
        assert not tooling_metrics.is_malformed()


class TestSConsToolingMetrics(unittest.TestCase):
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_artifact_dir",
           return_value='/test')
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_env_vars_dict",
           return_value={'env': 'env'})
    @patch("buildscripts.metrics.metrics_datatypes.BuildInfo._get_scons_options_dict",
           return_value={'opt': 'opt'})
    def test_scons_tooling_metrics_valid(self, mock_options, mock_env, mock_artifact_dir):
        parser = MagicMock()
        parser.parse_args = MagicMock(return_value={"opt1": "val1"})
        tooling_metrics = under_test.SConsToolingMetrics.generate_metrics(
            datetime.utcnow(),
            {'env': 'env'},
            {'opts': 'opts'},
            parser,
            ['test1', 'test2'],
            MOCK_EXIT_HOOK,
        )
        assert not tooling_metrics.is_malformed()

    def test_scons_tooling_metrics_malformed(self):
        tooling_metrics = under_test.SConsToolingMetrics.generate_metrics(
            datetime.utcnow(),
            {'env': 'env'},
            {'opts': 'opts'},
            None,
            [],
            MOCK_EXIT_HOOK,
        )
        assert tooling_metrics.is_malformed()
