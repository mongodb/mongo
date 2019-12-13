"""The unittest.TestCase instances for setting up and tearing down fixtures."""
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.testcases import interface
from buildscripts.resmokelib.utils import registry


class FixtureTestCase(interface.TestCase):  # pylint: disable=abstract-method
    """Base class for the fixture test cases."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, job_name, phase):
        """Initialize the FixtureTestCase."""
        interface.TestCase.__init__(self, logger, "Fixture test", "{}_fixture_{}".format(
            job_name, phase), dynamic=True)
        self.job_name = job_name


class FixtureSetupTestCase(FixtureTestCase):
    """TestCase for setting up a fixture."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED
    PHASE = "setup"

    def __init__(self, logger, fixture, job_name, times_set_up):
        """Initialize the FixtureSetupTestCase."""
        specific_phase = "{phase}_{times_set_up}".format(phase=self.PHASE,
                                                         times_set_up=times_set_up)
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


class FixtureKillTestCase(FixtureTestCase):
    """TestCase for killing a fixture. Intended for use before archiving a failed test."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED
    PHASE = "kill"

    def __init__(self, logger, fixture, job_name, times_set_up):
        """Initialize the FixtureKillTestCase."""
        specific_phase = "{phase}_{times_set_up}".format(phase=self.PHASE,
                                                         times_set_up=times_set_up)
        FixtureTestCase.__init__(self, logger, job_name, specific_phase)
        self.fixture = fixture

    def run_test(self):
        """Tear down the fixture."""
        try:
            self.return_code = 2  # Test return code of 2 is used for fixture failures.
            self.logger.info("Killing the fixture %s.", self.fixture)
            self.fixture.teardown(finished=False, kill=True)
            self.logger.info("Finished killing %s.", self.fixture)
            self.return_code = 0
        except:
            self.logger.exception("An error occurred while killing %s.", self.fixture)
            raise
