"""Test hook for cleaning up data files created by the fixture."""

import os

import pymongo

from buildscripts.resmokelib.testing.hooks import interface


class CleanEveryN(interface.Hook):
    """Restart the fixture after it has ran 'n' tests.

    On mongod-related fixtures, this will clear the dbpath.
    """

    IS_BACKGROUND = False

    DEFAULT_N = 20

    def __init__(
        self,
        hook_logger,
        fixture,
        n=DEFAULT_N,
        shell_options=None,
        skip_database_deletion=False,
        skip_fixture_restart=False,
    ):
        """Initialize CleanEveryN."""
        description = "CleanEveryN (restarts the fixture after running `n` tests)"
        interface.Hook.__init__(self, hook_logger, fixture, description)

        # Try to isolate what test triggers the leak by restarting the fixture each time.
        if "detect_leaks=1" in os.getenv("ASAN_OPTIONS", ""):
            self.logger.info(
                "ASAN_OPTIONS environment variable set to detect leaks, so restarting"
                " the fixture after each test instead of after every %d.",
                n,
            )
            n = 1

        self.n = n
        self.tests_run = 0
        self.shell_options = shell_options
        self.skip_database_deletion = skip_database_deletion
        self.skip_fixture_restart = skip_fixture_restart

    def after_test(self, test, test_report):
        """After test cleanup."""
        self.tests_run += 1
        if self.tests_run < self.n:
            if self.skip_database_deletion:
                return

            for cluster in self.fixture.get_independent_clusters():
                if self.shell_options and "authenticationMechanism" in self.shell_options:
                    client = pymongo.MongoClient(
                        cluster.get_driver_connection_url(),
                        username=self.shell_options["username"],
                        password=self.shell_options["password"],
                        authSource=self.shell_options["authenticationDatabase"],
                        authMechanism=self.shell_options["authenticationMechanism"],
                    )
                else:
                    client = pymongo.MongoClient(cluster.get_driver_connection_url())

                for db_name in client.list_database_names():
                    if db_name in ["admin", "config", "local", "$external"]:
                        continue
                    self.logger.info(
                        f"Dropping database to ensure it isn't validated again: {db_name}"
                    )
                    client.drop_database(db_name)
                    self.logger.info(f"Successfully dropped database: {db_name}")
            return

        if self.skip_fixture_restart:
            return
        hook_test_case = CleanEveryNTestCase.create_after_test(test.logger, test, self)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)


class CleanEveryNTestCase(interface.DynamicTestCase):
    """CleanEveryNTestCase class."""

    def run_test(self):
        """Execute test hook."""
        try:
            self.logger.info(
                "%d tests have been run against the fixture, stopping it...", self._hook.tests_run
            )
            self._hook.tests_run = 0

            self.fixture.teardown()

            self.logger.info("Starting the fixture back up again...")
            self.fixture.setup()
            self.fixture.await_ready()
        except:
            self.logger.exception("Encountered an error while restarting the fixture.")
            raise
