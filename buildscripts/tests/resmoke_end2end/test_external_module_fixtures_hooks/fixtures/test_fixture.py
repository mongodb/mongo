"""Test fixture for external module."""

from buildscripts.resmokelib.testing.fixtures import interface


class TestExternalFixture(interface.Fixture):
    """A simple test fixture for external module testing."""

    REGISTERED_NAME = "TestExternalFixture"

    def __init__(self, logger, job_num, fixturelib, dbpath_prefix=None):
        interface.Fixture.__init__(self, logger, job_num, fixturelib, dbpath_prefix=dbpath_prefix)

    def setup(self):
        """Setup the fixture."""
        pass

    def await_ready(self):
        """Wait for the fixture to be ready."""
        pass

    def teardown(self, finished=False, kill=False):
        """Teardown the fixture."""
        pass

    def is_running(self):
        """Check if the fixture is running."""
        return False
