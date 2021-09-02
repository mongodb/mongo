"""Utilities for constructing fixtures that may span multiple versions."""
import io
import os
import threading
from abc import ABC, abstractmethod
from git import Repo

import buildscripts.resmokelib.utils.registry as registry
import buildscripts.resmokelib.config as config
from buildscripts.resmokelib import errors, multiversionconstants
from buildscripts.resmokelib.utils import default_if_none
from buildscripts.resmokelib.utils import autoloader
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.testing.fixtures.interface import _FIXTURES

MONGO_REPO_LOCATION = "."
FIXTURE_DIR = "buildscripts/resmokelib/testing/fixtures"
RETRIEVE_DIR = "build/multiversionfixtures"
RETRIEVE_LOCK = threading.Lock()

_BUILDERS = {}  # type: ignore


def make_fixture(class_name, logger, job_num, *args, **kwargs):
    """Provide factory function for creating Fixture instances."""

    fixturelib = FixtureLib()

    if class_name in _BUILDERS:
        builder = _BUILDERS[class_name]()
        return builder.build_fixture(logger, job_num, fixturelib, *args, **kwargs)

    if class_name not in _FIXTURES:
        raise ValueError("Unknown fixture class '%s'" % class_name)
    return _FIXTURES[class_name](logger, job_num, fixturelib, *args, **kwargs)


class FixtureBuilder(ABC, metaclass=registry.make_registry_metaclass(_BUILDERS, type(ABC))):  # pylint: disable=invalid-metaclass
    """
    ABC for fixture builders.

    If any fixture has special logic for assembling different components
    (e.g. for multiversion), define a builder to handle it.
    """

    # For any subclass, set a REGISTERED_NAME corresponding to the fixture the class builds.
    REGISTERED_NAME = "Builder"

    @abstractmethod
    def build_fixture(self, logger, job_num, fixturelib, *args, **kwargs):
        """Abstract method to build a replica set."""
        return


class BinVersionEnum(object):
    """Enumeration version types."""

    OLD = 'old'
    NEW = 'new'


class ReplSetBuilder(FixtureBuilder):
    """Builder class for fixtures support replication."""

    REGISTERED_NAME = "ReplicaSetFixture"
    latest_class = "MongoDFixture"
    multiversion_class_suffix = "_multiversion_class_suffix"

    def build_fixture(self, logger, job_num, fixturelib, *args, **kwargs):  # pylint: disable=too-many-locals
        """Build a replica set."""
        # We hijack the mixed_bin_versions passed to the fixture.
        mixed_bin_versions = kwargs.pop("mixed_bin_versions", config.MIXED_BIN_VERSIONS)
        old_bin_version = kwargs.pop("old_bin_version", config.MULTIVERSION_BIN_VERSION)

        # We also hijack the num_nodes because we need it here.
        num_nodes = kwargs.pop("num_nodes", 2)
        num_replset_nodes = config.NUM_REPLSET_NODES
        num_nodes = num_replset_nodes if num_replset_nodes else num_nodes
        kwargs["num_nodes"] = num_nodes

        replset_config_options = kwargs.get("replset_config_options", {})
        mongod_executable = default_if_none(
            kwargs.get("mongod_executable"), config.MONGOD_EXECUTABLE,
            config.DEFAULT_MONGOD_EXECUTABLE)
        kwargs["mongod_executable"] = mongod_executable
        num_nodes = kwargs["num_nodes"]
        latest_mongod = mongod_executable

        fcv = multiversionconstants.LATEST_FCV

        executables = {BinVersionEnum.NEW: latest_mongod}
        classes = {BinVersionEnum.NEW: self.latest_class}

        # Default to NEW for all bin versions; may be overridden below.
        mongod_binary_versions = [BinVersionEnum.NEW for _ in range(num_nodes)]

        is_multiversion = mixed_bin_versions is not None
        if is_multiversion:
            old_shell_version = {
                config.MultiversionOptions.LAST_LTS:
                    multiversionconstants.LAST_LTS_MONGO_BINARY,
                config.MultiversionOptions.LAST_CONTINUOUS:
                    multiversionconstants.LAST_CONTINUOUS_MONGO_BINARY
            }[old_bin_version]

            old_mongod_version = {
                config.MultiversionOptions.LAST_LTS:
                    multiversionconstants.LAST_LTS_MONGOD_BINARY,
                config.MultiversionOptions.LAST_CONTINUOUS:
                    multiversionconstants.LAST_CONTINUOUS_MONGOD_BINARY
            }[old_bin_version]

            executables[BinVersionEnum.OLD] = old_mongod_version
            classes[BinVersionEnum.OLD] = f"{self.latest_class}{self.multiversion_class_suffix}"

            load_version(version_path_suffix=self.multiversion_class_suffix,
                         shell_path=old_shell_version)

            is_config_svr = "configsvr" in replset_config_options and replset_config_options[
                "configsvr"]

            if not is_config_svr:
                mongod_binary_versions = [x for x in mixed_bin_versions]
            else:

                # Our documented recommended path for upgrading shards lets us assume that config
                # server nodes will always be fully upgraded before shard nodes.
                mongod_binary_versions = [BinVersionEnum.NEW] * 2

            num_versions = len(mixed_bin_versions)
            fcv = {
                config.MultiversionOptions.LAST_LTS:
                    multiversionconstants.LAST_LTS_FCV, config.MultiversionOptions.LAST_CONTINUOUS:
                        multiversionconstants.LAST_CONTINUOUS_FCV
            }[old_bin_version]

            if num_versions != num_nodes and not is_config_svr:
                msg = (("The number of binary versions specified: {} do not match the number of"\
                        " nodes in the replica set: {}.")).format(num_versions, num_nodes)
                raise errors.ServerFailure(msg)

        replset = _FIXTURES[self.REGISTERED_NAME](logger, job_num, fixturelib, *args, **kwargs)

        replset.set_fcv(fcv)
        for node_index in range(replset.num_nodes):
            node = self._new_mongod(replset, node_index, executables, classes,
                                    mongod_binary_versions[node_index], is_multiversion)
            replset.install_mongod(node)

        if replset.start_initial_sync_node:
            if not replset.initial_sync_node:
                replset.initial_sync_node_idx = replset.num_nodes
                # TODO: This adds the linear chain and steady state param now, is that ok?
                replset.initial_sync_node = self._new_mongod(replset, replset.initial_sync_node_idx,
                                                             executables, classes,
                                                             BinVersionEnum.NEW, is_multiversion)

        return replset

    @classmethod
    def _new_mongod(cls, replset, replset_node_index, executables, classes, cur_version,
                    is_multiversion):
        # pylint: disable=too-many-arguments
        """Return a standalone.MongoDFixture configured to be used as replica-set member."""
        mongod_logger = replset.get_logger_for_mongod(replset_node_index)
        mongod_options = replset.get_options_for_mongod(replset_node_index)

        new_fixture_port = None
        old_fixture = None

        # There is more than one class for mongod, this means we're in multiversion mode.
        if is_multiversion:
            old_fixture = make_fixture(classes[BinVersionEnum.OLD], mongod_logger, replset.job_num,
                                       mongod_executable=executables[BinVersionEnum.OLD],
                                       mongod_options=mongod_options,
                                       preserve_dbpath=replset.preserve_dbpath)

            # Assign the same port for old and new fixtures so upgrade/downgrade can be done without
            # changing the replicaset config.
            new_fixture_port = old_fixture.port

        new_fixture_mongod_options = replset.get_options_for_mongod(replset_node_index)
        if config.ENABLED_FEATURE_FLAGS is not None:
            for ff in config.ENABLED_FEATURE_FLAGS:
                new_fixture_mongod_options["set_parameters"][ff] = True

        new_fixture = make_fixture(classes[BinVersionEnum.NEW], mongod_logger, replset.job_num,
                                   mongod_executable=executables[BinVersionEnum.NEW],
                                   mongod_options=new_fixture_mongod_options,
                                   preserve_dbpath=replset.preserve_dbpath, port=new_fixture_port)

        return FixtureContainer(new_fixture, old_fixture, cur_version)


def load_version(version_path_suffix=None, shell_path=None):
    """Load the last_lts/last_continuous fixtures."""
    with RETRIEVE_LOCK, registry.suffix(version_path_suffix):
        # Only one thread needs to retrieve the fixtures.
        retrieve_dir = os.path.relpath(os.path.join(RETRIEVE_DIR, version_path_suffix))
        if not os.path.exists(retrieve_dir):
            try:
                # Avoid circular import
                import buildscripts.resmokelib.run.generate_multiversion_exclude_tags as gen_tests
                commit = gen_tests.get_backports_required_hash_for_shell_version(
                    mongo_shell_path=shell_path)
            except FileNotFoundError as err:
                print("Error running the mongo shell, please ensure it's in your $PATH: ", err)
                raise
            retrieve_fixtures(retrieve_dir, commit)

        package_name = retrieve_dir.replace('/', '.')
        autoloader.load_all_modules(name=package_name, path=[retrieve_dir])  # type: ignore


def retrieve_fixtures(directory, commit):
    """Populate a directory with the fixture files corresponding to a commit."""
    repo = Repo(MONGO_REPO_LOCATION)
    real_commit = repo.commit(commit)
    tree = real_commit.tree / FIXTURE_DIR

    os.makedirs(directory, exist_ok=True)

    for blob in tree.blobs:
        output = os.path.join(directory, blob.name)
        with io.BytesIO(blob.data_stream.read()) as retrieved, open(output, "w") as file:
            file.write(retrieved.read().decode("utf-8"))


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
