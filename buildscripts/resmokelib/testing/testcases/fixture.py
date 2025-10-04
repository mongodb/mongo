"""The unittest.TestCase instances for setting up and tearing down fixtures."""

from pymongo import ReadPreference

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures.external import ExternalFixture
from buildscripts.resmokelib.testing.fixtures.replicaset import ReplicaSetFixture
from buildscripts.resmokelib.testing.testcases import interface
from buildscripts.resmokelib.utils import registry
from buildscripts.resmokelib.utils.sharded_cluster_util import (
    refresh_logical_session_cache_with_retry,
)


class FixtureTestCase(interface.TestCase):
    """Base class for the fixture test cases."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, job_name, phase):
        """Initialize the FixtureTestCase."""
        interface.TestCase.__init__(
            self, logger, "Fixture test", "{}_fixture_{}".format(job_name, phase), dynamic=True
        )
        self.job_name = job_name


class FixtureSetupTestCase(FixtureTestCase):
    """TestCase for setting up a fixture."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED
    PHASE = "setup"

    def __init__(self, logger, fixture, job_name, times_set_up):
        """Initialize the FixtureSetupTestCase."""
        specific_phase = "{phase}_{times_set_up}".format(
            phase=self.PHASE, times_set_up=times_set_up
        )
        FixtureTestCase.__init__(self, logger, job_name, specific_phase)
        self.fixture = fixture

    def run_test(self):
        """Set up the fixture and wait for it to be ready."""
        try:
            self.return_code = 2
            self.logger.info("Starting the setup of %s.", self.fixture)
            self.fixture.setup()
            self.logger.info("Waiting for %s to be ready.", self.fixture)
            self.fixture.await_ready()
            if (
                not isinstance(self.fixture, (fixture_interface.NoOpFixture, ExternalFixture))
                # TODO(SERVER-109851): Remove this.
                # disagg mongod does not yet support "refreshLogicalSessionCacheNow" because it requires
                # wtimeout support.
                and self.fixture.__class__.__name__ != "DisaggFixture"
                # Replica set with --configsvr cannot run refresh unless it is part of a sharded cluster.
                and not (
                    isinstance(self.fixture, ReplicaSetFixture)
                    and "configsvr" in self.fixture.mongod_options
                )
            ):
                mongo_client = self.fixture.mongo_client(ReadPreference.PRIMARY)
                # Read from the CSRS primary to gossip the most recent configTime to the mongos.
                # This ensures that the latest state of the sessions collection can be seen
                # by the router, when performing the LogicalSessionCache refresh.
                mongo_client.admin["system.version"].find({})
                # Perform the LogicalSessionCache refresh.
                refresh_logical_session_cache_with_retry(mongo_client)
            self.logger.info("Finished the setup of %s.", self.fixture)
            self.return_code = 0
        except errors.ServerFailure as err:
            self.logger.error("An error occurred during the setup of %s: %s", self.fixture, err)
            raise
        except:
            self.logger.exception("An error occurred during the setup of %s.", self.fixture)
            raise


class FixtureTeardownTestCase(FixtureTestCase):
    """TestCase for tearing down a fixture."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED
    PHASE = "teardown"

    def __init__(self, logger, fixture, job_name):
        """Initialize the FixtureTeardownTestCase."""
        FixtureTestCase.__init__(self, logger, job_name, self.PHASE)
        self.fixture = fixture

    def run_test(self):
        """Tear down the fixture."""
        try:
            self.return_code = 2
            self.logger.info("Starting the teardown of %s.", self.fixture)
            self.fixture.teardown(finished=True)
            self.logger.info("Finished the teardown of %s.", self.fixture)
            self.return_code = 0
        except errors.ServerFailure as err:
            self.logger.error("An error occurred during the teardown of %s: %s", self.fixture, err)
            raise
        except:
            self.logger.exception("An error occurred during the teardown of %s.", self.fixture)
            raise


class FixtureAbortTestCase(FixtureTestCase):
    """TestCase for killing a fixture. Intended for use before archiving a failed test."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED
    PHASE = "abort"

    def __init__(self, logger, fixture, job_name, times_set_up):
        """Initialize the FixtureAbortTestCase."""
        specific_phase = "{phase}_{times_set_up}".format(
            phase=self.PHASE, times_set_up=times_set_up
        )
        FixtureTestCase.__init__(self, logger, job_name, specific_phase)
        self.fixture = fixture

    def run_test(self):
        """Tear down the fixture."""
        try:
            self.return_code = 2  # Test return code of 2 is used for fixture failures.
            self.logger.info("Aborting the fixture %s due to test failure.", self.fixture)
            self.fixture.teardown(finished=False, mode=fixture_interface.TeardownMode.ABORT)
            self.logger.info("Finished aborting %s.", self.fixture)
            self.return_code = 0
        except errors.ServerFailure:
            # If the server wasn't already running, we can't exactly fail to abort it.
            self.logger.info("Finished aborting %s.", self.fixture)
            self.return_code = 0
        except:
            self.logger.exception("An error occurred while aborting %s.", self.fixture)
            raise
