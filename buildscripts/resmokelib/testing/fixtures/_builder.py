"""Utilities for constructing fixtures that may span multiple versions."""
import logging
import threading
from abc import ABC, abstractmethod
from typing import Any, Dict, List, Optional, Tuple, Type

from buildscripts.resmokelib import config
from buildscripts.resmokelib import logging
from buildscripts.resmokelib.utils import registry
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.testing.fixtures.interface import _FIXTURES
from buildscripts.resmokelib.suitesconfig import _get_suite_config
from buildscripts.resmokelib.testing import suite as _suite
from buildscripts.resmokelib.testing.fixtures.replicaset import \
    ReplicaSetFixture
from buildscripts.resmokelib.testing.fixtures.shardedcluster import \
    ShardedClusterFixture, _RouterView
from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture
from buildscripts.resmokelib.utils import autoloader, default_if_none, pick_catalog_shard_node

MONGO_REPO_LOCATION = "."
FIXTURE_DIR = "buildscripts/resmokelib/testing/fixtures"
RETRIEVE_DIR = "build/multiversionfixtures"
RETRIEVE_LOCK = threading.Lock()

_BUILDERS = {}  # type: ignore


def make_fixture(class_name, logger, job_num, *args, enable_feature_flags=True, **kwargs):
    """Provide factory function for creating Fixture instances."""

    fixturelib = FixtureLib()

    if class_name in _BUILDERS:
        builder = _BUILDERS[class_name]()
        return builder.build_fixture(logger, job_num, fixturelib, *args, **kwargs)

    if class_name not in _FIXTURES:
        raise ValueError("Unknown fixture class '%s'" % class_name)

    # Special case MongoDFixture or _MongosFixture for now since we only add one option.
    # If there's more logic, we should add a builder class for them.
    if class_name in ["MongoDFixture", "_MongoSFixture"] and enable_feature_flags:
        return _FIXTURES[class_name](logger, job_num, fixturelib, *args,
                                     add_feature_flags=bool(config.ENABLED_FEATURE_FLAGS), **kwargs)

    return _FIXTURES[class_name](logger, job_num, fixturelib, *args, **kwargs)


def make_dummy_fixture(suite_name):
    """Create a dummy fixture for the given suite.

    This fixture is not meant to be used for testing.

    This fixture should only be used to inspect the test topology for a given suite.
    """

    # Get info to create the target fixture
    suite = _suite.Suite(suite_name, _get_suite_config(suite_name))
    fixture_config = suite.get_executor_config()["fixture"]
    fixture_class = fixture_config.pop("class")

    # This is a noop. A job logger is expected to have been created in order to create a fixture.
    _ = logging.loggers.new_job_logger(suite.test_kind, job_num=0)

    fixture_logger = logging.loggers.new_fixture_logger(fixture_class, job_num=0)
    return make_fixture(fixture_class, fixture_logger, job_num=0, **fixture_config)


class FixtureBuilder(ABC, metaclass=registry.make_registry_metaclass(_BUILDERS, type(ABC))):  # pylint: disable=invalid-metaclass
    """
    ABC for fixture builders.

    If any fixture has special logic for assembling different components
    (e.g. for multiversion), define a builder to handle it.
    """

    # For any subclass, set a REGISTERED_NAME corresponding to the fixture the class builds.
    REGISTERED_NAME = "Builder"

    @abstractmethod
    def build_fixture(self, logger, job_num, fixturelib, *args, existing_nodes=None, **kwargs):
        """Abstract method to build a fixture."""
        return


class BinVersionEnum(object):
    """Enumeration version types."""

    OLD = 'old'
    NEW = 'new'


class FixtureContainer(object):
    """Provide automatic state change between old and new fixture."""

    attributes = ["_fixtures", "cur_version_cls", "get_cur_version"]

    def __init__(self, new_fixture, old_fixture=None, cur_version=None):
        """Initialize FixtureContainer."""

        if old_fixture is not None:
            self._fixtures = {BinVersionEnum.NEW: new_fixture, BinVersionEnum.OLD: old_fixture}
            self.cur_version_cls = self._fixtures[cur_version]
        else:
            # No need to support dictionary of fixture classes if only a single version of
            # fixtures is used.
            self._fixtures = None
            self.cur_version_cls = new_fixture

    def change_version_if_needed(self, node):
        """
        Upgrade or downgrade the fixture version to be different to that of `node`.

        @returns a boolean of whether the version was changed.
        """
        if self.cur_version_cls == node.get_cur_version():
            for ver, cls in self._fixtures.items():
                if ver != node.get_cur_version():
                    self.cur_version_cls = cls
            return True
        else:
            return False

    def get_cur_version(self):
        """Get current fixture version from FixtureContainer."""
        return self.cur_version_cls

    def __getattr__(self, name):
        return self.cur_version_cls.__getattribute__(name)

    def __setattr__(self, key, value):
        if key in FixtureContainer.attributes:
            return object.__setattr__(self, key, value)
        else:
            return self.cur_version_cls.__setattr__(key, value)


def _extract_multiversion_options(
        kwargs: Dict[str, Any]) -> Tuple[Optional[List[str]], Optional[str]]:
    """Pop multiversion options from kwargs dict and return them.

    :param kwargs: fixture kwargs
    :return: a tuple with the list of bin versions and the old bin version
    """

    mixed_bin_versions = kwargs.pop("mixed_bin_versions", config.MIXED_BIN_VERSIONS)
    # We have the same code in configure_resmoke.py to split config.MIXED_BIN_VERSIONS,
    # but here it is for the case, when it comes from resmoke suite definition
    if isinstance(mixed_bin_versions, str):
        mixed_bin_versions = mixed_bin_versions.split("_")
    if config.MIXED_BIN_VERSIONS is None:
        config.MIXED_BIN_VERSIONS = mixed_bin_versions

    old_bin_version = kwargs.pop("old_bin_version", config.MULTIVERSION_BIN_VERSION)
    if config.MULTIVERSION_BIN_VERSION is None:
        config.MULTIVERSION_BIN_VERSION = old_bin_version

    return mixed_bin_versions, old_bin_version


class ReplSetBuilder(FixtureBuilder):
    """Builder class for fixtures support replication."""

    REGISTERED_NAME = "ReplicaSetFixture"
    LATEST_MONGOD_CLASS = "MongoDFixture"

    def build_fixture(self, logger: logging.Logger, job_num: int, fixturelib: Type[FixtureLib],
                      *args, existing_nodes: Optional[List[MongoDFixture]] = None,
                      **kwargs) -> ReplicaSetFixture:
        """Build a replica set.

        :param logger: fixture logger
        :param job_num: current job number
        :param fixturelib: an instance of resmokelib API class
        :param existing_nodes: the list of mongod fixtures
        :return: configured replica set fixture
        """

        launch_mongot = bool("launch_mongot" in kwargs)
        self._mutate_kwargs(kwargs)
        mixed_bin_versions, old_bin_version = _extract_multiversion_options(kwargs)
        self._validate_multiversion_options(kwargs, mixed_bin_versions)
        mongod_class, mongod_executables, mongod_binary_versions = self._get_mongod_assets(
            kwargs, mixed_bin_versions, old_bin_version)

        replset = _FIXTURES[self.REGISTERED_NAME](logger, job_num, fixturelib, *args, **kwargs)

        is_multiversion = mixed_bin_versions is not None
        fcv = self._get_fcv(is_multiversion, old_bin_version)
        replset.set_fcv(fcv)

        # Don't build new nodes if existing nodes are provided.
        if existing_nodes:
            # Rename the logger to improve readability when printing node info maps
            for idx, node in enumerate(existing_nodes):
                node.logger = replset.get_logger_for_mongod(idx)
                replset.install_mongod(node)
            return replset

        for node_index in range(replset.num_nodes):
            node = self._new_mongod(replset, node_index, mongod_executables, mongod_class,
                                    mongod_binary_versions[node_index], is_multiversion,
                                    launch_mongot)
            replset.install_mongod(node)

        if replset.start_initial_sync_node:
            if not replset.initial_sync_node:
                replset.initial_sync_node_idx = replset.num_nodes
                replset.initial_sync_node = self._new_mongod(
                    replset, replset.initial_sync_node_idx, mongod_executables, mongod_class,
                    BinVersionEnum.NEW, is_multiversion, launch_mongot)

        return replset

    @staticmethod
    def _mutate_kwargs(kwargs: Dict[str, Any]) -> None:
        """Update replica set fixture kwargs.

        :param kwargs: replica set fixture kwargs
        """
        num_nodes = kwargs.pop("num_nodes", 2)
        num_nodes = config.NUM_REPLSET_NODES if config.NUM_REPLSET_NODES else num_nodes
        kwargs["num_nodes"] = num_nodes

        mongod_executable = default_if_none(
            kwargs.get("mongod_executable"), config.MONGOD_EXECUTABLE,
            config.DEFAULT_MONGOD_EXECUTABLE)
        kwargs["mongod_executable"] = mongod_executable

    @staticmethod
    def _validate_multiversion_options(kwargs: Dict[str, Any],
                                       mixed_bin_versions: Optional[List[str]]) -> None:
        """Error out if the number of binary versions does not match the number of nodes in replica set.

        :param kwargs: sharded cluster fixture kwargs
        :param mixed_bin_versions: the list of bin versions
        """
        if mixed_bin_versions is not None:
            num_versions = len(mixed_bin_versions)
            replset_config_options = kwargs.get("replset_config_options", {})
            is_config_svr = "configsvr" in replset_config_options and replset_config_options[
                "configsvr"]

            if num_versions != kwargs["num_nodes"] and not is_config_svr:
                msg = ("The number of binary versions specified: {} do not match the number of"
                       " nodes in the replica set: {}.").format(num_versions, kwargs["num_nodes"])
                raise errors.ServerFailure(msg)

    @classmethod
    def _get_mongod_assets(
            cls, kwargs: Dict[str, Any], mixed_bin_versions: Optional[List[str]],
            old_bin_version: Optional[str]) -> Tuple[Dict[str, str], Dict[str, str], List[str]]:
        """Make dicts with mongod new/old class and executable names and binary versions.

        :param kwargs: sharded cluster fixture kwargs
        :param mixed_bin_versions: the list of bin versions
        :param old_bin_version: old bin version
        :return: tuple with dicts that contain mongod new/old class and executable names
                 and the list of binary versions
        """
        executables = {BinVersionEnum.NEW: kwargs["mongod_executable"]}
        _class = cls.LATEST_MONGOD_CLASS

        # Default to NEW for all bin versions; may be overridden below.
        binary_versions = [BinVersionEnum.NEW for _ in range(kwargs["num_nodes"])]

        if mixed_bin_versions is not None:
            from buildscripts.resmokelib import multiversionconstants

            old_mongod_version = {
                config.MultiversionOptions.LAST_LTS:
                    multiversionconstants.LAST_LTS_MONGOD_BINARY,
                config.MultiversionOptions.LAST_CONTINUOUS:
                    multiversionconstants.LAST_CONTINUOUS_MONGOD_BINARY,
            }[old_bin_version]

            executables[BinVersionEnum.OLD] = old_mongod_version
            binary_versions = [x for x in mixed_bin_versions]

        return _class, executables, binary_versions

    @staticmethod
    def _get_fcv(is_multiversion: bool, old_bin_version: Optional[str]) -> str:
        """Get FCV.

        :param is_multiversion: whether we are in multiversion mode
        :param old_bin_version: old bin version
        :return: FCV
        """
        from buildscripts.resmokelib import multiversionconstants

        fcv = multiversionconstants.LATEST_FCV
        if is_multiversion:
            fcv = {
                config.MultiversionOptions.LAST_LTS:
                    multiversionconstants.LAST_LTS_FCV,
                config.MultiversionOptions.LAST_CONTINUOUS:
                    multiversionconstants.LAST_CONTINUOUS_FCV,
            }[old_bin_version]

        return fcv

    @staticmethod
    def _new_mongod(replset: ReplicaSetFixture, replset_node_index: int,
                    executables: Dict[str, str], _class: str, cur_version: str,
                    is_multiversion: bool, launch_mongot: bool) -> FixtureContainer:
        """Make a fixture container with configured mongod fixture(s) in it.

        In non-multiversion mode only a new mongod fixture will be in the fixture container.
        In multiversion mode a new mongod and the old mongod fixtures will be in the container.

        :param replset: replica set fixture
        :param replset_node_index: the index of node in replica set
        :param executables: dict with a new and the old (if multiversion) mongod executable names
        :param _class: str with the mongod fixture name
        :param cur_version: old or new version
        :param is_multiversion: whether we are in multiversion mode
        :return: fixture container with configured mongod fixture(s) in it
        """
        mongod_logger = replset.get_logger_for_mongod(replset_node_index)
        mongod_options = replset.get_options_for_mongod(replset_node_index)

        new_fixture_port = None
        old_fixture = None

        if is_multiversion:
            # We do not run old versions with feature flags enabled
            old_fixture = make_fixture(
                _class, mongod_logger, replset.job_num, enable_feature_flags=False,
                mongod_executable=executables[BinVersionEnum.OLD], mongod_options=mongod_options,
                preserve_dbpath=replset.preserve_dbpath)

            # Assign the same port for old and new fixtures so upgrade/downgrade can be done without
            # changing the replicaset config.
            new_fixture_port = old_fixture.port

        new_fixture_mongod_options = replset.get_options_for_mongod(replset_node_index)
        new_fixture = make_fixture(_class, mongod_logger, replset.job_num,
                                   mongod_executable=executables[BinVersionEnum.NEW],
                                   mongod_options=new_fixture_mongod_options,
                                   preserve_dbpath=replset.preserve_dbpath, port=new_fixture_port,
                                   launch_mongot=launch_mongot)

        return FixtureContainer(new_fixture, old_fixture, cur_version)


def get_package_name(dir_path: str) -> str:
    """Evaluate python package name from relative directory path.

    :param dir_path: relative directory path
    :return: python package name
    """
    return dir_path.replace('/', '.').replace("\\", ".")


class ShardedClusterBuilder(FixtureBuilder):
    """Builder class for sharded cluster fixtures."""

    REGISTERED_NAME = "ShardedClusterFixture"
    LATEST_MONGOS_CLASS = "_MongoSFixture"

    def build_fixture(self, logger: logging.Logger, job_num: int, fixturelib: Type[FixtureLib],
                      *args, **kwargs) -> ShardedClusterFixture:
        """Build a sharded cluster.

        :param logger: fixture logger
        :param job_num: current job number
        :param fixturelib: an instance of resmokelib API class
        :return: configured sharded cluster fixture
        """
        self._mutate_kwargs(kwargs)
        mixed_bin_versions, old_bin_version = _extract_multiversion_options(kwargs)
        is_multiversion = mixed_bin_versions is not None
        self._validate_multiversion_options(kwargs, mixed_bin_versions)
        self._validate_embedded_router_mode_options(kwargs, is_multiversion)

        mongos_class, mongos_executables = self._get_mongos_assets(kwargs, mixed_bin_versions,
                                                                   old_bin_version)

        sharded_cluster = _FIXTURES[self.REGISTERED_NAME](logger, job_num, fixturelib, *args,
                                                          **kwargs)

        config_shard = kwargs["config_shard"]
        config_svr = None
        # We install the configsvr before the shards, so that embedded-router shards can know the
        # config-server connection string when they are created. Since config servers do not
        # currently hold collection data, a mongot enabled shared cluster doesn't couple/launch
        # the config server with an accompanying mongot
        if config_shard is None:
            config_svr = self._new_configsvr(sharded_cluster, is_multiversion, old_bin_version)
        else:
            config_svr = self._new_rs_shard(sharded_cluster, mixed_bin_versions, old_bin_version,
                                            config_shard, kwargs["num_rs_nodes_per_shard"],
                                            launch_mongot=False)
        sharded_cluster.install_configsvr(config_svr)

        launch_mongot = bool("launch_mongot" in kwargs)
        for rs_shard_index in range(kwargs["num_shards"]):
            if rs_shard_index != config_shard:
                rs_shard = self._new_rs_shard(sharded_cluster, mixed_bin_versions, old_bin_version,
                                              rs_shard_index, kwargs["num_rs_nodes_per_shard"],
                                              launch_mongot)
                sharded_cluster.install_rs_shard(rs_shard)
            else:
                sharded_cluster.install_rs_shard(config_svr)

        num_routers = kwargs["num_mongos"]
        shardsvrs = sharded_cluster.get_shardsvrs()

        def install_router():
            if not kwargs.get("embedded_router", None):
                mongos = self._new_mongos(sharded_cluster, mongos_executables, mongos_class,
                                          mongos_index, num_routers, is_multiversion)
                sharded_cluster.install_mongos(mongos)
            else:
                router_view = self._new_router_view(sharded_cluster, mongos_index, num_routers,
                                                    shardsvrs.pop())
                sharded_cluster.install_mongos(router_view)

        for mongos_index in range(num_routers):
            install_router()

        return sharded_cluster

    @staticmethod
    def _mutate_kwargs(kwargs: Dict[str, Any]) -> None:
        """Update sharded cluster fixture kwargs.

        :param kwargs: sharded cluster fixture kwargs
        """
        num_shards = kwargs.pop("num_shards", 1)
        num_shards = num_shards if not config.NUM_SHARDS else config.NUM_SHARDS
        kwargs["num_shards"] = num_shards

        num_rs_nodes_per_shard = kwargs.pop("num_rs_nodes_per_shard", 1)
        num_rs_nodes_per_shard = num_rs_nodes_per_shard if not config.NUM_REPLSET_NODES else config.NUM_REPLSET_NODES
        kwargs["num_rs_nodes_per_shard"] = num_rs_nodes_per_shard

        num_mongos = kwargs.pop("num_mongos", 1)
        kwargs["num_mongos"] = num_mongos

        mongos_executable = default_if_none(
            kwargs.get("mongos_executable"), config.MONGOS_EXECUTABLE,
            config.DEFAULT_MONGOS_EXECUTABLE)
        kwargs["mongos_executable"] = mongos_executable

        config_shard = pick_catalog_shard_node(
            kwargs.pop("config_shard", config.CONFIG_SHARD), num_shards)
        # Currently the auto_boostrap_procedure requires us to have a config_shard
        if "use_auto_bootstrap_procedure" in kwargs and kwargs[
                "use_auto_bootstrap_procedure"] and not config_shard:
            config_shard = 0
        kwargs["embedded_router"] = kwargs.pop("embedded_router", config.EMBEDDED_ROUTER)
        kwargs["config_shard"] = config_shard

    @staticmethod
    def _validate_multiversion_options(kwargs: Dict[str, Any],
                                       mixed_bin_versions: Optional[List[str]]) -> None:
        """Error out if the number of binary versions does not match the number of nodes in sharded cluster.

        :param kwargs: sharded cluster fixture kwargs
        :param mixed_bin_versions: the list of bin versions
        """
        if mixed_bin_versions is not None:
            len_versions = len(mixed_bin_versions)
            num_mongods = kwargs["num_shards"] * kwargs["num_rs_nodes_per_shard"]

            if len_versions != num_mongods:
                msg = ("The number of binary versions specified: {} do not match the number of"
                       " nodes in the sharded cluster: {}.").format(len_versions, num_mongods)
                raise errors.ServerFailure(msg)

    @staticmethod
    def _validate_embedded_router_mode_options(kwargs: Dict[str, Any],
                                               is_multiversion: bool) -> None:
        """Raise an exception if the configuration for the sharded cluster can't support embedded_router_mode.

        :param kwargs: sharded cluster fixture kwargs.
        :param is_multiversion: True if this is a multiversion test. 
        """
        # TODO SERVER-81458: Support multiversion testing with embedded routers.
        # TODO SERVER-81459: Support testing a cluster with a combination of dedicated and embedded routers.
        embedded_router_mode = kwargs.get("embedded_router", None)
        num_routers = kwargs["num_mongos"]
        num_shardsvrs = kwargs["num_shards"] * kwargs["num_rs_nodes_per_shard"]
        if embedded_router_mode:
            if num_routers > num_shardsvrs:
                raise ValueError(
                    "When running in embedded router mode, num_mongos must be <= the total number of shardsvrs in the cluster."
                )
            if is_multiversion:
                raise ValueError("Embedded router mode does not support multiversion testing.")

    @classmethod
    def _get_mongos_assets(cls, kwargs: Dict[str, Any], mixed_bin_versions: Optional[List[str]],
                           old_bin_version: Optional[str]) -> Tuple[Dict[str, str], Dict[str, str]]:
        """Make dicts with mongos new/old class and executable names.

        :param kwargs: sharded cluster fixture kwargs
        :param mixed_bin_versions: the list of bin versions
        :param old_bin_version: old bin version
        :return: tuple with dicts that contain mongos new/old class and executable names
        """

        executables = {BinVersionEnum.NEW: kwargs.pop("mongos_executable")}
        _class = cls.LATEST_MONGOS_CLASS

        if mixed_bin_versions is not None:
            from buildscripts.resmokelib import multiversionconstants

            old_mongos_version = {
                config.MultiversionOptions.LAST_LTS:
                    multiversionconstants.LAST_LTS_MONGOS_BINARY,
                config.MultiversionOptions.LAST_CONTINUOUS:
                    multiversionconstants.LAST_CONTINUOUS_MONGOS_BINARY,
            }[old_bin_version]

            executables[BinVersionEnum.OLD] = old_mongos_version
        return _class, executables

    @staticmethod
    def _new_configsvr(sharded_cluster: ShardedClusterFixture, is_multiversion: bool,
                       old_bin_version: Optional[str]) -> ReplicaSetFixture:
        """Return a replica set fixture configured as the config server.

        :param sharded_cluster: sharded cluster fixture we are configuring config server for
        :param is_multiversion: whether we are in multiversion mode
        :param old_bin_version: old bin version
        :return: replica set fixture configured as the config server
        """

        configsvr_logger = sharded_cluster.get_configsvr_logger()
        configsvr_kwargs = sharded_cluster.get_configsvr_kwargs()

        mixed_bin_versions = None
        if is_multiversion:
            # Our documented recommended path for upgrading shards lets us assume that config
            # server nodes will always be fully upgraded before shard nodes.
            mixed_bin_versions = [BinVersionEnum.NEW] * 2

        return make_fixture("ReplicaSetFixture", configsvr_logger, sharded_cluster.job_num,
                            mixed_bin_versions=mixed_bin_versions, old_bin_version=old_bin_version,
                            **configsvr_kwargs)

    @staticmethod
    def _new_rs_shard(sharded_cluster: ShardedClusterFixture,
                      mixed_bin_versions: Optional[List[str]], old_bin_version: Optional[str],
                      rs_shard_index: int, num_rs_nodes_per_shard: int,
                      launch_mongot: bool) -> ReplicaSetFixture:
        """Return a replica set fixture configured as a shard in a sharded cluster.

        :param sharded_cluster: sharded cluster fixture we are configuring config server for
        :param mixed_bin_versions: the list of bin versions
        :param old_bin_version: old bin version
        :param rs_shard_index: replica set shard index
        :param num_rs_nodes_per_shard: the number of nodes in a replica set per shard
        :return: replica set fixture configured as a shard in a sharded cluster
        """

        rs_shard_logger = sharded_cluster.get_rs_shard_logger(rs_shard_index)
        rs_shard_kwargs = sharded_cluster.get_rs_shard_kwargs(rs_shard_index)
        rs_shard_kwargs["launch_mongot"] = launch_mongot

        if mixed_bin_versions is not None:
            start_index = rs_shard_index * num_rs_nodes_per_shard
            mixed_bin_versions = mixed_bin_versions[start_index:start_index +
                                                    num_rs_nodes_per_shard]

        return make_fixture("ReplicaSetFixture", rs_shard_logger, sharded_cluster.job_num,
                            num_nodes=num_rs_nodes_per_shard, mixed_bin_versions=mixed_bin_versions,
                            old_bin_version=old_bin_version, **rs_shard_kwargs)

    @staticmethod
    def _new_mongos(sharded_cluster: ShardedClusterFixture, executables: Dict[str, str],
                    _class: str, mongos_index: int, total: int,
                    is_multiversion: bool) -> FixtureContainer:
        """Make a fixture container with configured mongos fixture(s) in it.

        In non-multiversion mode only a new mongos fixture will be in the fixture container.
        In multiversion mode a new mongos and the old mongos fixtures will be in the container.

        :param sharded_cluster: sharded cluster fixture we are configuring mongos for
        :param executables: dict with a new and the old (if multiversion) mongos executable names
        :param _class: str with the mongos fixture name
        :param mongos_index: the index of mongos
        :param total: total number of mongos
        :param is_multiversion: whether we are in multiversion mode
        :return: fixture container with configured mongos fixture(s) in it
        """

        mongos_logger = sharded_cluster.get_mongos_logger(mongos_index, total)
        mongos_kwargs = sharded_cluster.get_mongos_kwargs()

        old_fixture = None

        if is_multiversion:
            # We do not run old versions with feature flags enabled
            old_fixture = make_fixture(
                _class, mongos_logger, sharded_cluster.job_num, enable_feature_flags=False,
                mongos_executable=executables[BinVersionEnum.OLD], **mongos_kwargs)

        # We can't restart mongos since explicit ports are not supported.
        new_fixture_mongos_kwargs = sharded_cluster.get_mongos_kwargs()
        new_fixture = make_fixture(_class, mongos_logger, sharded_cluster.job_num,
                                   mongos_executable=executables[BinVersionEnum.NEW],
                                   **new_fixture_mongos_kwargs)

        # Always spin up an old mongos if in multiversion mode given mongos is the last thing in the update path.
        return FixtureContainer(new_fixture, old_fixture,
                                BinVersionEnum.OLD if is_multiversion else BinVersionEnum.NEW)

    @staticmethod
    def _new_router_view(sharded_cluster: ShardedClusterFixture, mongos_index: int, total: int,
                         mongod: MongoDFixture) -> _RouterView:
        """Make a fixture that allows ShardedClusterFixture to treat a shardsvr as a router."""

        router_logger = sharded_cluster.get_mongos_logger(mongos_index, total)
        router_kwargs = {}
        router_kwargs["mongod"] = mongod

        fix = make_fixture("_RouterView", router_logger, sharded_cluster.job_num, **router_kwargs)
        return fix
