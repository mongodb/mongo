"""Facade wrapping the resmokelib dependencies used by fixtures."""
from typing import Dict

from buildscripts.resmokelib import config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib import logging
from buildscripts.resmokelib.core import network
from buildscripts.resmokelib.utils.dictionary import merge_dicts
from buildscripts.resmokelib.utils.history import make_historic as _make_historic
from buildscripts.resmokelib.testing.fixtures import _builder


class FixtureLib:
    """Class that exposes the resmokelib API that fixtures can use."""

    #################
    # Logger tools #
    #################

    def assert_logger(self, logger):
        """Assert that the given logger has the correct type."""
        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

    def close_loggers(self, handler):
        """Add 'handler' to the queue so that it is closed later by the flush thread.

        Return the scheduled event which may be used for later cancelation (see cancel()).
        """
        return logging.flush.close_later(handler)

    def new_fixture_node_logger(self, fixture_class, job_num, node_name):
        """Create a logger for a particular element in a multi-process fixture."""
        return logging.loggers.new_fixture_node_logger(fixture_class, job_num, node_name)

    ############
    # Programs #
    ############

    def make_fixture(self, class_name, logger, job_num, *args, **kwargs):
        """Build fixtures by calling builder API."""
        return _builder.make_fixture(class_name, logger, job_num, *args, **kwargs)

    def mongod_program(self, logger, job_num, executable, process_kwargs, mongod_options):
        """
        Return a Process instance that starts mongod arguments constructed from 'mongod_options'.

        @param logger - The logger to pass into the process.
        @param executable - The mongod executable to run.
        @param process_kwargs - A dict of key-value pairs to pass to the process.
        @param mongod_options - A HistoryDict describing the various options to pass to the mongod.
        """
        return core.programs.mongod_program(logger, job_num, executable, process_kwargs,
                                            mongod_options)

    def mongos_program(self, logger, job_num, executable=None, process_kwargs=None,
                       mongos_options=None):
        """Return a Process instance that starts a mongos with arguments constructed from 'kwargs'."""
        return core.programs.mongos_program(logger, job_num, executable, process_kwargs,
                                            mongos_options)

    def generic_program(self, logger, args, process_kwargs=None, **kwargs):
        """Return a Process instance that starts an arbitrary executable.

        The executable arguments are constructed from 'kwargs'.

        The args parameter is an array of strings containing the command to execute.
        """
        return core.programs.generic_program(logger, args, process_kwargs, **kwargs)

    #########
    # Utils #
    #########

    ServerFailure = errors.ServerFailure

    def make_historic(self, obj):
        """Convert a python object into a corresponding Historic to track history."""
        return _make_historic(obj)

    def default_if_none(self, *values):
        """Return the first argument that is not 'None'."""
        return utils.default_if_none(*values)

    def get_config(self):
        """Return an objects whose attributes are fixture config values."""
        return _FixtureConfig()

    def get_next_port(self, job_num):
        """Return the next available port that fixture can use."""
        return network.PortAllocator.next_fixture_port(job_num)

    SET_PARAMETERS_KEY = "set_parameters"

    def merge_mongo_option_dicts(self, original: Dict, override: Dict):
        """
        Merge mongod/s options such that --setParameter is merged recursively.

        Values from `original` are replaced in-place with those of `override` where they exist.
        """
        original_set_parameters = original.get(self.SET_PARAMETERS_KEY, {})
        override_set_parameters = override.get(self.SET_PARAMETERS_KEY, {})

        merged_set_parameters = merge_dicts(original_set_parameters, override_set_parameters)
        original.update(override)
        original[self.SET_PARAMETERS_KEY] = merged_set_parameters

        return original


class _FixtureConfig(object):
    """Class that stores fixture configuration info."""

    def __init__(self):
        """Initialize FixtureConfig, setting values."""
        from buildscripts.resmokelib.multiversionconstants import LAST_LTS_MONGOD_BINARY, LAST_LTS_MONGOS_BINARY, LAST_CONTINUOUS_MONGOD_BINARY, LAST_CONTINUOUS_MONGOS_BINARY

        # pylint: disable=invalid-name
        self.MONGOD_EXECUTABLE = config.MONGOD_EXECUTABLE
        self.DEFAULT_MONGOD_EXECUTABLE = config.DEFAULT_MONGOD_EXECUTABLE
        self.MONGOD_SET_PARAMETERS = config.MONGOD_SET_PARAMETERS
        self.FIXTURE_SUBDIR = config.FIXTURE_SUBDIR
        self.AUTO_KILL = config.AUTO_KILL
        self.ALWAYS_USE_LOG_FILES = config.ALWAYS_USE_LOG_FILES
        self.LAST_LTS_MONGOD_BINARY = LAST_LTS_MONGOD_BINARY
        self.LAST_LTS_MONGOS_BINARY = LAST_LTS_MONGOS_BINARY
        self.LAST_CONTINUOUS_MONGOD_BINARY = LAST_CONTINUOUS_MONGOD_BINARY
        self.LAST_CONTINUOUS_MONGOS_BINARY = LAST_CONTINUOUS_MONGOS_BINARY
        self.USE_LEGACY_MULTIVERSION = config.USE_LEGACY_MULTIVERSION
        self.ENABLED_FEATURE_FLAGS = config.ENABLED_FEATURE_FLAGS
        self.EVERGREEN_TASK_ID = config.EVERGREEN_TASK_ID
        self.FLOW_CONTROL = config.FLOW_CONTROL
        self.FLOW_CONTROL_TICKETS = config.FLOW_CONTROL_TICKETS
        self.MAJORITY_READ_CONCERN = config.MAJORITY_READ_CONCERN
        self.NO_JOURNAL = config.NO_JOURNAL
        self.STORAGE_ENGINE = config.STORAGE_ENGINE
        self.STORAGE_ENGINE_CACHE_SIZE = config.STORAGE_ENGINE_CACHE_SIZE
        self.TRANSPORT_LAYER = config.TRANSPORT_LAYER
        self.WT_COLL_CONFIG = config.WT_COLL_CONFIG
        self.WT_ENGINE_CONFIG = config.WT_ENGINE_CONFIG
        self.WT_INDEX_CONFIG = config.WT_INDEX_CONFIG
        self.MIXED_BIN_VERSIONS = config.MIXED_BIN_VERSIONS
        self.LINEAR_CHAIN = config.LINEAR_CHAIN
        self.NUM_REPLSET_NODES = config.NUM_REPLSET_NODES
        self.NUM_SHARDS = config.NUM_SHARDS
        self.DEFAULT_MONGOS_EXECUTABLE = config.DEFAULT_MONGOS_EXECUTABLE
        self.MONGOS_EXECUTABLE = config.MONGOS_EXECUTABLE
        self.MONGOS_SET_PARAMETERS = config.MONGOS_SET_PARAMETERS
        self.DBPATH_PREFIX = config.DBPATH_PREFIX
        self.DEFAULT_DBPATH_PREFIX = config.DEFAULT_DBPATH_PREFIX
