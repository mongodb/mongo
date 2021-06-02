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

USE_LEGACY_MULTIVERSION = True

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


class ReplSetBuilder(FixtureBuilder):
    """Builder class for fixtures support replication."""

    REGISTERED_NAME = "ReplicaSetFixture"

    def build_fixture(self, logger, job_num, fixturelib, *args, **kwargs):  # pylint: disable=too-many-locals
        """Build a replica set."""
        # We hijack the mixed_bin_versions passed to the fixture.
        mixed_bin_versions = kwargs.pop("mixed_bin_versions", config.MIXED_BIN_VERSIONS)
        if USE_LEGACY_MULTIVERSION:
            # We mark the use of the legacy multiversion system by allowing
            # access to mixed_bin_versions.
            kwargs["mixed_bin_versions"] = mixed_bin_versions

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
        latest_class = "MongoDFixture"
        executables = []
        classes = []
        fcv = None

        lts_class_suffix = "_last_lts"

        if mixed_bin_versions is None:
            executables = [latest_mongod for x in range(num_nodes)]
            classes = [latest_class for x in range(num_nodes)]
        else:

            is_config_svr = "configsvr" in replset_config_options and replset_config_options[
                "configsvr"]
            if USE_LEGACY_MULTIVERSION:
                executables = [
                    latest_mongod if (x == "new") else multiversionconstants.LAST_LTS_MONGOD_BINARY
                    for x in mixed_bin_versions
                ]
                classes = [
                    latest_class if (x == "new") else f"{latest_class}{lts_class_suffix}"
                    for x in mixed_bin_versions
                ]
            else:
                load_version(version_path_suffix=lts_class_suffix,
                             shell_path=multiversionconstants.LAST_LTS_MONGO_BINARY)

                if not is_config_svr:
                    executables = [
                        latest_mongod if
                        (x == "new") else multiversionconstants.LAST_LTS_MONGOD_BINARY
                        for x in mixed_bin_versions
                    ]
                    classes = [
                        latest_class if (x == "new") else f"{latest_class}{lts_class_suffix}"
                        for x in mixed_bin_versions
                    ]
            if is_config_svr:
                # Our documented recommended path for upgrading shards lets us assume that config
                # server nodes will always be fully upgraded before shard nodes.
                executables = [latest_mongod, latest_mongod]
                classes = [latest_class, latest_class]

            num_versions = len(mixed_bin_versions)
            fcv = multiversionconstants.LAST_LTS_FCV

            if num_versions != num_nodes and not is_config_svr:
                msg = (("The number of binary versions specified: {} do not match the number of"\
                        " nodes in the replica set: {}.")).format(num_versions, num_nodes)
                raise errors.ServerFailure(msg)

        replset = _FIXTURES[self.REGISTERED_NAME](logger, job_num, fixturelib, *args, **kwargs)

        replset.set_fcv(fcv)
        for i in range(replset.num_nodes):
            node = self._new_mongod(replset, i, executables[i], classes[i])
            replset.install_mongod(node)

        if replset.start_initial_sync_node:
            if not replset.initial_sync_node:
                replset.initial_sync_node_idx = replset.num_nodes
                # TODO: This adds the linear chain and steady state param now, is that ok?
                replset.initial_sync_node = self._new_mongod(replset, replset.initial_sync_node_idx,
                                                             latest_mongod, latest_class)

        return replset

    @classmethod
    def _new_mongod(cls, replset, index, executable, mongod_class):  # TODO Not a class method
        """Return a standalone.MongoDFixture configured to be used as replica-set member."""
        mongod_logger = replset.get_logger_for_mongod(index)
        mongod_options = replset.get_options_for_mongod(index)

        steady_state_constraint_param = "oplogApplicationEnforcesSteadyStateConstraints"
        if (steady_state_constraint_param not in mongod_options["set_parameters"]
                and mongod_class == "MongoDFixture"):
            mongod_options["set_parameters"][steady_state_constraint_param] = True
        # legacy multiversion line
        if mongod_class == "MongoDFixture_last_lts":
            mongod_class = "MongoDFixture"

        return make_fixture(mongod_class, mongod_logger, replset.job_num,
                            mongod_executable=executable, mongod_options=mongod_options,
                            preserve_dbpath=replset.preserve_dbpath)


def load_version(version_path_suffix=None, shell_path=None):
    """Load the last_lts fixtures."""
    with RETRIEVE_LOCK, registry.suffix(version_path_suffix):
        # Only one thread needs to retrieve the fixtures.
        retrieve_dir = os.path.relpath(os.path.join(RETRIEVE_DIR, version_path_suffix))
        if not os.path.exists(retrieve_dir):
            try:
                # Avoud circular import
                import buildscripts.evergreen_gen_multiversion_tests as gen_tests
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
