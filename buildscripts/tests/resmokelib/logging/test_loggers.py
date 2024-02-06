"""Unit tests for the buildscripts.resmokelib.logging.loggers module."""

import unittest
from unittest.mock import MagicMock

from buildscripts.resmokelib.logging import loggers
from buildscripts.resmokelib import config

# pylint: disable=protected-access


class TestLoggers(unittest.TestCase):
    """Unit tests for the resmoke loggers."""

    def setUp(self):
        loggers.ROOT_EXECUTOR_LOGGER = MagicMock()
        loggers.ROOT_FIXTURE_LOGGER = MagicMock()
        loggers.ROOT_TESTS_LOGGER = MagicMock()
        loggers.BUILDLOGGER_SERVER = MagicMock()
        loggers._get_buildlogger_handler_info = MagicMock()
        config.LOGGING_CONFIG = MagicMock()

    def test_resmoke_logger(self):
        logger = loggers.new_resmoke_logger()
        self.assertEqual(logger.parent, loggers.ROOT_EXECUTOR_LOGGER)

    def test_job_logger(self):
        loggers.BUILDLOGGER_SERVER.new_build_id.return_value = 78
        logger = loggers.new_job_logger("dummy_test_kind", 55)
        self.assertEqual(loggers._BUILD_ID_REGISTRY[55], 78)
        self.assertEqual(logger.parent, loggers.ROOT_EXECUTOR_LOGGER)

    def test_fixture_logger(self):
        loggers._BUILD_ID_REGISTRY[32] = 29
        loggers._get_buildlogger_handler_info.return_value = True
        mock_handler = MagicMock()
        loggers.BUILDLOGGER_SERVER.get_global_handler.return_value = mock_handler
        logger = loggers.new_fixture_logger("dummy_class", 32)
        self.assertEqual(logger.handlers[0], mock_handler)

    def test_fixture_node_logger(self):
        mock_fixture_logger = MagicMock()
        loggers._FIXTURE_LOGGER_REGISTRY[24] = mock_fixture_logger
        logger = loggers.new_fixture_node_logger("dummy_class", 24, "dummy_node")
        self.assertEqual(logger.parent, mock_fixture_logger)

    def test_testqueue_logger(self):
        logger = loggers.new_testqueue_logger("dummy_test_kind")
        self.assertEqual(logger.parent, loggers.ROOT_TESTS_LOGGER)

    def test_test_logger(self):
        loggers._BUILD_ID_REGISTRY[88] = 47
        loggers._get_buildlogger_handler_info.return_value = True
        mock_handler = MagicMock()
        loggers.BUILDLOGGER_SERVER.get_test_handler.return_value = mock_handler
        loggers.BUILDLOGGER_SERVER.new_test_id.return_value = 22
        loggers.BUILDLOGGER_SERVER.get_test_log_url.return_value = "dummy_url"

        mock_parent = MagicMock()
        (logger, url) = loggers.new_test_logger("dummy_shortname", "dummy_basename",
                                                "dummy_command", mock_parent, 88, 99, MagicMock())
        self.assertEqual(logger.handlers[0], mock_handler)
        self.assertEqual(logger.parent, mock_parent)
        self.assertEqual(url, "dummy_url")

    def test_test_thread_logger(self):
        logger = loggers.new_test_thread_logger("dummy_parent", "dummy_kind", "dummy_id")
        self.assertEqual(logger.parent, "dummy_parent")

    def test_hook_logger(self):
        loggers._FIXTURE_LOGGER_REGISTRY[22] = "dummy_fixture_logger"
        logger = loggers.new_hook_logger("dummy_class", 22)
        self.assertEqual(logger.parent, "dummy_fixture_logger")

    def test_shorten_logger_name(self):
        config.SHORTEN_LOGGER_NAME_CONFIG = {
            "remove": ["MongoDFixture", "ReplicaSetFixture", "ShardedClusterFixture"],
            "replace": {
                "primary": "prim",
                "secondary": "sec",
                "node": "n",
                "shard": "s",
                "configsvr": "c",
                "mongos": "s",
                "job": "j",
            },
        }
        transform = [
            ("MongoDFixture:job0", "j0"),
            ("ReplicaSetFixture:job0:primary", "j0:prim"),
            ("ReplicaSetFixture:job0:secondary", "j0:sec"),
            ("ReplicaSetFixture:job0:secondary0", "j0:sec0"),
            ("ReplicaSetFixture:job0:node0", "j0:n0"),
            ("ShardedClusterFixture:job0:mongos", "j0:s"),
            ("ShardedClusterFixture:job0:configsvr:primary", "j0:c:prim"),
            ("ShardedClusterFixture:job0:configsvr:secondary", "j0:c:sec"),
            ("ShardedClusterFixture:job0:configsvr:secondary0", "j0:c:sec0"),
            ("ShardedClusterFixture:job0:shard0:primary", "j0:s0:prim"),
            ("ShardedClusterFixture:job0:shard0:secondary", "j0:s0:sec"),
            ("ShardedClusterFixture:job0:shard0:secondary0", "j0:s0:sec0"),
        ]
        for full_name, expected_short_name in transform:
            short_name = loggers._shorten(full_name)
            self.assertEqual(short_name, expected_short_name)
