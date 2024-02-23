"""Interface of the different fixtures for executing JSTests against."""

from logging import Logger
import os.path
import time
from enum import Enum
from collections import namedtuple
from typing import List, TYPE_CHECKING

import pymongo
import pymongo.errors

from buildscripts.resmokelib.utils import registry

# TODO: if we ever fix the circular deps in resmoke we will be able to get rid of this
if TYPE_CHECKING:
    from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib

_VERSIONS = {}  # type: ignore

# FIXTURE_API_VERSION versions the API presented by interface.py,
# registry.py, and fixturelib.py. Increment this when making
# changes to them. Follow semantic versioning so that back-branch
# fixtures are compatible when new features are added.


# Note for multiversion fixtures: The formal API version here describes what
# features a fixture can expect resmokelib to provide (e.g. new_fixture_node_logger
# in fixturelib). It also provides a definition of _minimum_ functionality
# (defined in the Fixture base class) that resmoke at large can expect from a fixture.
# It is possible for fixtures to define additional public members beyond the minimum
# in the Fixture base class, for use in hooks, builders, etc. (e.g. the initial
# sync node in ReplicaSetFixture). These form an informal API of their own, which has
# less of a need to be formalized because we expect major changes to it to occur on the
# current master, allowing backwards-compatibility. On the other hand, the
# interface.py and fixturelib API establishes forward-compatibility of fixture files.
# If the informal API becomes heavily used and needs forward-compatibility,
# consider adding it to the formal API.
class APIVersion(object, metaclass=registry.make_registry_metaclass(_VERSIONS)):  # pylint: disable=invalid-metaclass
    """Class storing fixture API version info."""

    REGISTERED_NAME = "APIVersion"

    FIXTURE_API_VERSION = "0.1.0"

    @classmethod
    def check_api_version(cls, actual):
        """Check that we are compatible with the actual API version."""

        def to_major(version):
            return int(version.split(".")[0])

        def to_minor(version):
            return int(version.split(".")[1])

        expected = cls.FIXTURE_API_VERSION
        return to_major(expected) == to_major(actual) and to_minor(expected) <= to_minor(actual)


_FIXTURES = {}  # type: ignore


class TeardownMode(Enum):
    """
    Enumeration representing different ways a fixture can be torn down.

    Each constant has the value of a Linux signal, even though the signal won't be used on Windows.
    This class is used because the 'signal' package on Windows has different values.
    """

    TERMINATE = 15
    KILL = 9
    ABORT = 6


class Fixture(object, metaclass=registry.make_registry_metaclass(_FIXTURES)):  # pylint: disable=invalid-metaclass
    """Base class for all fixtures."""

    # Error response codes copied from mongo/base/error_codes.yml.
    _WRITE_CONCERN_FAILED = 64
    _NODE_NOT_FOUND = 74
    _NEW_REPLICA_SET_CONFIGURATION_INCOMPATIBLE = 103
    _CONFIGURATION_IN_PROGRESS = 109
    _CURRENT_CONFIG_NOT_COMMITTED_YET = 308
    _INTERRUPTED_DUE_TO_REPL_STATE_CHANGE = 11602
    _INTERRUPTED_DUE_TO_STORAGE_CHANGE = 355

    # We explicitly set the 'REGISTERED_NAME' attribute so that PyLint realizes that the attribute
    # is defined for all subclasses of Fixture.
    REGISTERED_NAME = "Fixture"

    AWAIT_READY_TIMEOUT_SECS = 300

    def __init__(self, logger: Logger, job_num: int, fixturelib: 'FixtureLib',
                 dbpath_prefix: str = None):
        """Initialize the fixture with a logger instance."""

        self.fixturelib = fixturelib
        self.config = self.fixturelib.get_config()

        self.fixturelib.assert_logger(logger)

        if not isinstance(job_num, int):
            raise TypeError("job_num must be an integer")
        elif job_num < 0:
            raise ValueError("job_num must be a nonnegative integer")

        self.logger = logger
        self.job_num = job_num

        dbpath_prefix = self.fixturelib.default_if_none(self.config.DBPATH_PREFIX, dbpath_prefix)
        dbpath_prefix = self.fixturelib.default_if_none(dbpath_prefix,
                                                        self.config.DEFAULT_DBPATH_PREFIX)
        self._dbpath_prefix = os.path.join(dbpath_prefix, "job{}".format(self.job_num))

    def pids(self):
        """Return any pids owned by this fixture."""
        raise NotImplementedError("pids must be implemented by Fixture subclasses %s" % self)

    def setup(self):
        """Create the fixture."""
        pass

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        pass

    def teardown(self, finished=False, mode=None):  # noqa
        """Destroy the fixture.

        The fixture's logging handlers are closed if 'finished' is true,
        which should happen when setup() won't be called again.

        Raises:
            errors.ServerFailure: If the teardown is not successful.
        """

        try:
            self._do_teardown(mode=mode)
        finally:
            if finished:
                for handler in self.logger.handlers:
                    # We ignore the cancellation token returned by close_later() since we always
                    # want the logs to eventually get flushed.
                    self.fixturelib.close_loggers(handler)

    def _do_teardown(self, mode=None):  # noqa
        """Destroy the fixture.

        This method must be implemented by subclasses.

        Raises:
            errors.ServerFailure: If the teardown is not successful.
        """
        pass

    def is_running(self):
        """Return true if the fixture is still operating and more tests and can be run."""
        return True

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        return []

    def get_dbpath_prefix(self):
        """Return dbpath prefix."""
        return self._dbpath_prefix

    def get_path_for_archival(self):
        """
        Return the dbpath for archival that includes all possible directories.

        This includes directories for resmoke fixtures and fixtures spawned by the shell.
        """
        return self._dbpath_prefix

    def get_internal_connection_string(self):
        """Return the connection string for this fixture.

        This is NOT a driver connection string, but a connection string of the format
        expected by the mongo::ConnectionString class.
        """
        raise NotImplementedError(
            "get_internal_connection_string must be implemented by Fixture subclasses")

    def get_shell_connection_url(self):
        """Return the connection string to be used by the mongo shell process executing a jstest.

        Defaults to returning the driver connection url, but can be overriden to provide 
        shell-specific options (such as using a gRPC port).
        https://docs.mongodb.com/manual/reference/connection-string/
        """
        return self.get_driver_connection_url()

    def get_driver_connection_url(self):
        """Return the mongodb connection string as defined below.

        https://docs.mongodb.com/manual/reference/connection-string/
        """
        raise NotImplementedError(
            "get_driver_connection_url must be implemented by Fixture subclasses")

    def mongo_client(self, read_preference=pymongo.ReadPreference.PRIMARY, timeout_millis=30000,
                     **kwargs):
        """Return a pymongo.MongoClient connecting to this fixture with specified 'read_preference'.

        The PyMongo driver will wait up to 'timeout_millis' milliseconds
        before concluding that the server is unavailable.
        """

        kwargs["connectTimeoutMS"] = timeout_millis
        if pymongo.version_tuple[0] >= 3:
            kwargs["serverSelectionTimeoutMS"] = timeout_millis
            kwargs["connect"] = True

        if self.config.TLS_MODE == "requireTLS":
            # When server is in 'tlsMode == requireTLS`,
            # the client would be unable to connect
            # without local TLS being explicitly turned on.
            # Other modes, such as 'allowTLS' permit both tls and non-tls clients.
            kwargs["tls"] = True
            kwargs["tlsAllowInvalidHostnames"] = True
            if self.config.TLS_CA_FILE:
                kwargs["tlsCAFile"] = self.config.TLS_CA_FILE
            if self.config.SHELL_TLS_CERTIFICATE_KEY_FILE:
                kwargs["tlsCertificateKeyFile"] = self.config.SHELL_TLS_CERTIFICATE_KEY_FILE

        return pymongo.MongoClient(host=self.get_driver_connection_url(),
                                   read_preference=read_preference, **kwargs)

    def __str__(self):
        return "%s (Job #%d)" % (self.__class__.__name__, self.job_num)

    def __repr__(self):
        return "%r(%r, %r)" % (self.__class__.__name__, self.logger, self.job_num)


class DockerComposeException(Exception):
    """Exception to use when there is a failure in the docker compose interface."""

    pass


class _DockerComposeInterface:
    """
    Implement the `_all_mongo_d_s_t_instances` method which returns all `mongo{d,s,t}` instances.

    Fixtures that use this interface can programmatically generate `docker-compose.yml` configurations
    by leveraging the `all_processes` method to access the startup args.
    """

    def _all_mongo_d_s_t(self) -> List[Fixture]:
        """
        Return a list of all mongo{d,s,t} `Fixture` instances in this fixture.

        :return: A list of `mongo{d,s,t}` `Fixture` instances.
        """
        raise NotImplementedError(
            "_all_mongo_d_s_t_instances must be implemented by Fixture subclasses that support `docker-compose.yml` generation."
        )

    def all_processes(self) -> List['Process']:
        """
        Return a list of all `mongo{d,s,t}` `Process` instances in the fixture.

        :return: A list of mongo{d,s,t} processes for the current fixture.
        """
        if not self.config.DOCKER_COMPOSE_BUILD_IMAGES:
            raise DockerComposeException(
                "This method is reserved for `--dockerComposeBuildImages` only.")

        processes = []

        # If `mongo_d_s.NOOP_MONGO_D_S_PROCESSES=True`, `mongo_d_s.setup()` will setup a dummy process
        # to extract args from instead of a real `mongo{d,s}`.
        for mongo_d_s in self._all_mongo_d_s_t():
            if mongo_d_s.__class__.__name__ == "MongoDFixture":
                mongo_d_s.setup()
                processes += [mongo_d_s.mongod]
            elif mongo_d_s.__class__.__name__ == "_MongoSFixture":
                mongo_d_s.setup()
                processes += [mongo_d_s.mongos]
            else:
                raise NotImplementedError(
                    f"Support for this class has not yet been added to docker compose: {mongo_d_s.__class__.__name__}"
                )
        return processes


class MultiClusterFixture(Fixture):
    """
    Base class for fixtures that may consist of multiple independent participant clusters.

    The participant clusters can function independently without coordination, but are bound together
    only for some duration as they participate in some process such as a migration. The participant
    clusters are fixtures themselves.
    """

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED  # type: ignore

    def get_independent_clusters(self):
        """Return a list of the independent clusters (fixtures) that participate in this fixture."""
        raise NotImplementedError(
            "get_independent_clusters must be implemented by MultiClusterFixture subclasses")


class ReplFixture(Fixture):
    """Base class for all fixtures that support replication."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED  # type: ignore

    AWAIT_REPL_TIMEOUT_MINS = 5
    AWAIT_REPL_TIMEOUT_FOREVER_MINS = 24 * 60

    def get_primary(self):
        """Return the primary of a replica set."""
        raise NotImplementedError("get_primary must be implemented by ReplFixture subclasses")

    def get_secondaries(self):
        """Return a list containing the secondaries of a replica set."""
        raise NotImplementedError("get_secondaries must be implemented by ReplFixture subclasses")

    def retry_until_wtimeout(self, insert_fn):
        """Retry until wtimeout reached.

        Given a callback function representing an insert operation on
        the primary, handle any connection failures, and keep retrying
        the operation for up to 'AWAIT_REPL_TIMEOUT_MINS' minutes.

        The insert operation callback should take an argument for the
        number of remaining seconds to provide as the timeout for the
        operation.
        """

        deadline = time.time() + ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60

        while True:
            try:
                remaining = deadline - time.time()
                insert_fn(remaining)
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    message = "Failed to connect to {} within {} minutes".format(
                        self.get_driver_connection_url(), ReplFixture.AWAIT_REPL_TIMEOUT_MINS)
                    self.logger.error(message)
                    raise self.fixturelib.ServerFailure(message)
            except pymongo.errors.WTimeoutError:
                message = "Replication of write operation timed out."
                self.logger.error(message)
                raise self.fixturelib.ServerFailure(message)
            except pymongo.errors.PyMongoError as err:
                message = "Write operation on {} failed: {}".format(
                    self.get_driver_connection_url(), err)
                raise self.fixturelib.ServerFailure(message)


class NoOpFixture(Fixture):
    """A Fixture implementation that does not start any servers.

    Used when the MongoDB deployment is started by the JavaScript test itself with MongoRunner,
    ReplSetTest, or ShardingTest.
    """

    REGISTERED_NAME = "NoOpFixture"

    def pids(self):
        """:return: any pids owned by this fixture (none for NopFixture)."""
        return []

    def mongo_client(self, read_preference=None, timeout_millis=None, **kwargs):
        """Return the mongo_client connection."""
        raise NotImplementedError("NoOpFixture does not support a mongo_client")

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        return None

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return None


class FixtureTeardownHandler(object):
    """A helper class used to teardown nodes inside a cluster and keep track of errors."""

    def __init__(self, logger):
        """Initialize a FixtureTeardownHandler.

        Args:
            logger: A logger to use to log teardown activity.
        """
        self._logger = logger
        self._success = True
        self._message = None

    def was_successful(self):
        """Indicate whether the teardowns performed by this instance were all successful."""
        return self._success

    def get_error_message(self):
        """Retrieve the combined error message for all the teardown failures.

        Return None if all the teardowns were successful.
        """
        return self._message

    def teardown(self, fixture, name, mode=None):  # noqa: D406,D407,D411,D413
        """Tear down the given fixture and log errors instead of raising a ServerFailure exception.

        Args:
            fixture: The fixture to tear down.
            name: The name of the fixture.
        Returns:
            True if the teardown is successful, False otherwise.
        """
        try:
            self._logger.info("Stopping %s...", name)
            fixture.teardown(mode=mode)
            self._logger.info("Successfully stopped %s.", name)
            return True
        except fixture.fixturelib.ServerFailure as err:
            msg = "Error while stopping {}: {}".format(name, err)
            self._logger.warning(msg)
            self._add_error_message(msg)
            self._success = False
            return False

    def _add_error_message(self, message):
        if not self._message:
            self._message = message
        else:
            self._message = "{} - {}".format(self._message, message)


def create_fixture_table(fixture):
    """Get fixture node info, make it a pretty table. Return it or None if fixture is invalid target."""
    info: List[NodeInfo] = fixture.get_node_info()
    if not info:
        return None

    columns = {}
    longest = {}
    for key in NodeInfo._fields:
        longest[key] = len(key)
        columns[key] = []
        for node in info:
            attr = getattr(node, key)
            str_value = str(attr) if attr is not None else '-'
            columns[key].append(str_value)
            longest[key] = max(longest[key], len(str_value))

    # Filter out columns where no row has a value
    columns = {k: v for k, v in columns.items() if not all(x == '-' for x in v)}

    def horizontal_separator():
        row = ""
        for key in columns:
            row += "+" + "-" * (longest[key])
        row += "+"
        return row

    def title_row():
        row = ""
        for key in columns:
            row += "|" + key + " " * (longest[key] - len(key))
        row += "|"
        return row

    def data_row(i):
        row = ""
        for key in columns:
            row += "|" + columns[key][i] + " " * (longest[key] - len(columns[key][i]))
        row += "|"
        return row

    table = ""
    table += horizontal_separator() + "\n"
    table += title_row() + "\n"
    table += horizontal_separator() + "\n"
    for i in range(len(info)):
        table += data_row(i) + "\n"
    table += horizontal_separator()

    return "Fixture status:\n" + table


def build_client(node, auth_options=None, read_preference=pymongo.ReadPreference.PRIMARY):
    """Authenticate client for the 'authenticationDatabase' and return the client."""
    if auth_options is not None:
        return node.mongo_client(
            username=auth_options["username"], password=auth_options["password"],
            authSource=auth_options["authenticationDatabase"],
            authMechanism=auth_options["authenticationMechanism"], read_preference=read_preference)
    else:
        return node.mongo_client(read_preference=read_preference)


# Represents a row in a node info table.
NodeInfo = namedtuple('NodeInfo', ['full_name', 'name', 'port', 'pid', 'router_port'])
