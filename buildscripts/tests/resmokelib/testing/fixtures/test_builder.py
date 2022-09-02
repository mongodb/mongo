"""Unit tests for the resmokelib.testing.fixtures._builder module."""
# pylint: disable=protected-access,invalid-name
import unittest
import filecmp
import os
from unittest.mock import MagicMock

from buildscripts.resmokelib import logging, parser, config
from buildscripts.resmokelib.core import network
from buildscripts.resmokelib.testing.fixtures import _builder as under_test

TEST_COMMIT = "1de5826097917875f48ca1ea4f2e53b40139f9ff"
TEST_SUFFIX = "_unittest_suffix"
TEST_RETRIEVE_DIR = os.path.join(under_test.RETRIEVE_DIR, TEST_SUFFIX)
SET_PARAMS = "set_parameters"


class TestRetrieveFixtures(unittest.TestCase):
    """Class that test retrieve_fixtures methods."""

    def test_retrieve_fixtures(self):
        """function to test retrieve_fixtures"""
        expected_standalone = os.path.join("buildscripts", "tests", "resmokelib", "testing",
                                           "fixtures", "retrieved_fixture.txt")
        under_test.retrieve_fixtures(TEST_RETRIEVE_DIR, TEST_COMMIT)
        retrieved_standalone = os.path.join(TEST_RETRIEVE_DIR, "standalone.py")
        self.assertTrue(
            filecmp.cmpfiles(retrieved_standalone, expected_standalone,
                             ["standalone.py", "retrieved_fixture.txt"], shallow=False))


class TestGetPackageName(unittest.TestCase):
    def test_get_package_name_from_posix_path(self):
        path = "build/multiversionfixtures/_unittest_suffix"
        package_name = under_test.get_package_name(path)
        self.assertEqual(package_name, "build.multiversionfixtures._unittest_suffix")

    def test_get_package_name_from_windows_path(self):
        path = "build\\multiversionfixtures\\_unittest_suffix"
        package_name = under_test.get_package_name(path)
        self.assertEqual(package_name, "build.multiversionfixtures._unittest_suffix")


class TestBuildShardedCluster(unittest.TestCase):
    original_constants = {}
    fixture_class_name = "ShardedClusterFixture"
    mock_logger = None
    job_num = 0

    @classmethod
    def setUpClass(cls):
        under_test.retrieve_fixtures(TEST_RETRIEVE_DIR, TEST_COMMIT)
        cls.original_constants["MULTIVERSION_CLASS_SUFFIX"] = under_test.MULTIVERSION_CLASS_SUFFIX
        under_test.MULTIVERSION_CLASS_SUFFIX = TEST_SUFFIX

        cls.mock_logger = MagicMock(spec=logging.Logger)
        logging.loggers._FIXTURE_LOGGER_REGISTRY[cls.job_num] = cls.mock_logger

    def tearDown(self):
        network.PortAllocator.reset()

    @classmethod
    def tearDownClass(cls):
        under_test.MULTIVERSION_CLASS_SUFFIX = cls.original_constants["MULTIVERSION_CLASS_SUFFIX"]

    def test_build_sharded_cluster_simple(self):
        parser.set_run_options()
        fixture_config = {"mongod_options": {SET_PARAMS: {"enableTestCommands": 1}}}
        sharded_cluster = under_test.make_fixture(self.fixture_class_name, self.mock_logger,
                                                  self.job_num, **fixture_config)

        self.assertEqual(len(sharded_cluster.configsvr.nodes), 1)
        self.assertEqual(len(sharded_cluster.shards), 1)
        self.assertEqual(len(sharded_cluster.shards[0].nodes), 1)
        self.assertEqual(len(sharded_cluster.mongos), 1)
        from buildscripts.resmokelib import multiversionconstants
        self.assertEqual(sharded_cluster.shards[0].fcv, multiversionconstants.LATEST_FCV)

    def test_build_sharded_cluster_with_feature_flags(self):
        ff_name = "featureFlagDummy"
        parser.set_run_options(f"--additionalFeatureFlags={ff_name}")
        fixture_config = {"mongod_options": {SET_PARAMS: {"enableTestCommands": 1}}}
        sharded_cluster = under_test.make_fixture(self.fixture_class_name, self.mock_logger,
                                                  self.job_num, **fixture_config)

        self.assertEqual(len(sharded_cluster.configsvr.nodes), 1)
        self.assertEqual(len(sharded_cluster.shards), 1)
        self.assertEqual(len(sharded_cluster.shards[0].nodes), 1)
        self.assertEqual(len(sharded_cluster.mongos), 1)
        from buildscripts.resmokelib import multiversionconstants
        self.assertEqual(sharded_cluster.shards[0].fcv, multiversionconstants.LATEST_FCV)
        # feature flags are set
        self.assertIn(ff_name, sharded_cluster.configsvr.nodes[0].mongod_options[SET_PARAMS])
        self.assertTrue(sharded_cluster.configsvr.nodes[0].mongod_options[SET_PARAMS][ff_name])
        self.assertIn(ff_name, sharded_cluster.shards[0].nodes[0].mongod_options[SET_PARAMS])
        self.assertTrue(sharded_cluster.shards[0].nodes[0].mongod_options[SET_PARAMS][ff_name])
        self.assertIn(ff_name, sharded_cluster.mongos[0].mongos_options[SET_PARAMS])
        self.assertTrue(sharded_cluster.mongos[0].mongos_options[SET_PARAMS][ff_name])

    def test_build_sharded_cluster_multiversion(self):
        parser.set_run_options()
        fixture_config = {
            "mongod_options": {SET_PARAMS: {"enableTestCommands": 1}},
            "configsvr_options": {"num_nodes": 2},
            "num_shards": 2,
            "num_rs_nodes_per_shard": 2,
            "mixed_bin_versions": "new_old_old_new",
            "old_bin_version": "last_lts",
        }
        sharded_cluster = under_test.make_fixture(self.fixture_class_name, self.mock_logger,
                                                  self.job_num, **fixture_config)

        self.assertEqual(len(sharded_cluster.configsvr.nodes), 2)
        self.assertEqual(len(sharded_cluster.shards), 2)
        self.assertEqual(len(sharded_cluster.shards[0].nodes), 2)
        self.assertEqual(len(sharded_cluster.shards[1].nodes), 2)
        self.assertEqual(len(sharded_cluster.mongos), 1)

        from buildscripts.resmokelib import multiversionconstants
        # configsvr nodes are always latest
        self.assertEqual(sharded_cluster.configsvr.nodes[0].mongod_executable,
                         config.DEFAULT_MONGOD_EXECUTABLE)
        self.assertEqual(sharded_cluster.configsvr.nodes[1].mongod_executable,
                         config.DEFAULT_MONGOD_EXECUTABLE)
        # 1st repl set nodes are latest and last-lts (new_old)
        self.assertEqual(sharded_cluster.shards[0].nodes[0].mongod_executable,
                         config.DEFAULT_MONGOD_EXECUTABLE)
        self.assertEqual(sharded_cluster.shards[0].nodes[1].mongod_executable,
                         multiversionconstants.LAST_LTS_MONGOD_BINARY)
        self.assertEqual(sharded_cluster.shards[0].fcv, multiversionconstants.LAST_LTS_FCV)
        # 2st repl set nodes are last-lts and latest (old_new)
        self.assertEqual(sharded_cluster.shards[1].nodes[0].mongod_executable,
                         multiversionconstants.LAST_LTS_MONGOD_BINARY)
        self.assertEqual(sharded_cluster.shards[1].nodes[1].mongod_executable,
                         config.DEFAULT_MONGOD_EXECUTABLE)
        self.assertEqual(sharded_cluster.shards[0].fcv, multiversionconstants.LAST_LTS_FCV)
        # mongos is last-lts
        self.assertEqual(sharded_cluster.mongos[0].mongos_executable,
                         multiversionconstants.LAST_LTS_MONGOS_BINARY)

    def test_build_sharded_cluster_multiversion_with_feature_flags(self):
        ff_name = "featureFlagDummy"
        parser.set_run_options(f"--additionalFeatureFlags={ff_name}")
        fixture_config = {
            "mongod_options": {SET_PARAMS: {"enableTestCommands": 1}},
            "configsvr_options": {"num_nodes": 2},
            "num_shards": 2,
            "num_rs_nodes_per_shard": 2,
            "mixed_bin_versions": "new_old_old_new",
            "old_bin_version": "last_lts",
        }
        sharded_cluster = under_test.make_fixture(self.fixture_class_name, self.mock_logger,
                                                  self.job_num, **fixture_config)

        self.assertEqual(len(sharded_cluster.configsvr.nodes), 2)
        self.assertEqual(len(sharded_cluster.shards), 2)
        self.assertEqual(len(sharded_cluster.shards[0].nodes), 2)
        self.assertEqual(len(sharded_cluster.shards[1].nodes), 2)
        self.assertEqual(len(sharded_cluster.mongos), 1)
        # feature flags are set on new versions
        self.assertIn(ff_name, sharded_cluster.configsvr.nodes[0].mongod_options[SET_PARAMS])
        self.assertTrue(sharded_cluster.configsvr.nodes[0].mongod_options[SET_PARAMS][ff_name])
        self.assertIn(ff_name, sharded_cluster.configsvr.nodes[1].mongod_options[SET_PARAMS])
        self.assertTrue(sharded_cluster.configsvr.nodes[1].mongod_options[SET_PARAMS][ff_name])
        self.assertIn(ff_name, sharded_cluster.shards[0].nodes[0].mongod_options[SET_PARAMS])
        self.assertTrue(sharded_cluster.shards[0].nodes[0].mongod_options[SET_PARAMS][ff_name])
        self.assertIn(ff_name, sharded_cluster.shards[1].nodes[1].mongod_options[SET_PARAMS])
        self.assertTrue(sharded_cluster.shards[1].nodes[1].mongod_options[SET_PARAMS][ff_name])
        # feature flags are NOT set on old versions
        self.assertNotIn(ff_name, sharded_cluster.shards[0].nodes[1].mongod_options[SET_PARAMS])
        self.assertNotIn(ff_name, sharded_cluster.shards[1].nodes[0].mongod_options[SET_PARAMS])
        self.assertNotIn(ff_name, sharded_cluster.mongos[0].mongos_options[SET_PARAMS])
