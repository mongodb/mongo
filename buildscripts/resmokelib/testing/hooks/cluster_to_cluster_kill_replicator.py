"""Test hook that continuously kills and restarts the replicator."""

import random
import time

import buildscripts.resmokelib.testing.fixtures.interface as interface
from buildscripts.resmokelib.testing.hooks import bghook


class KillReplicator(bghook.BGHook):
    """A hook that kills the replicator process and restarts it."""

    def __init__(self, hook_logger, fixture, tests_per_cycle, min_sleep_secs=1, max_sleep_secs=3):
        """Initialize KillReplicator."""
        bghook.BGHook.__init__(self, hook_logger, fixture, "Kill replicator hook", tests_per_cycle)
        self.min_sleep_secs = min_sleep_secs
        self.max_sleep_secs = max_sleep_secs

    def run_action(self):
        """Sleep for a random amount of time, then kill and restart the replicator."""
        rand_sleep = random.uniform(self.min_sleep_secs, self.max_sleep_secs)
        self.logger.info("Sleeping for %.2f seconds before killing the replicator", rand_sleep)
        time.sleep(rand_sleep)

        self.fixture.replicator.stop(interface.TeardownMode.KILL)
        self.fixture.replicator.setup()
