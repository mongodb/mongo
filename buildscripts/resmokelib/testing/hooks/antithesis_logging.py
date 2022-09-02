"""Hook that prints Antithesis commands to be executed in the Antithesis evironment."""

from time import sleep
from buildscripts.resmokelib.testing.hooks import interface


class AntithesisLogging(interface.Hook):
    """Prints antithesis commands before & after test run."""

    DESCRIPTION = "Prints antithesis commands before & after test run."

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture):
        """Initialize the AntithesisLogging hook."""
        interface.Hook.__init__(self, hook_logger, fixture, AntithesisLogging.DESCRIPTION)

    def before_test(self, test, test_report):
        """Ensure the fault injector is running before a test."""
        print("ANTITHESIS-COMMAND: Start Fault Injector")
        sleep(5)

    def after_test(self, test, test_report):
        """Ensure the fault injector is stopped after a test."""
        print("ANTITHESIS-COMMAND: Stop Fault Injector")
        sleep(5)
