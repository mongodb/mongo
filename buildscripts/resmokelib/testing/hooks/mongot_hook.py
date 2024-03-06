"""Test hook that removes mongot data directories."""

import shutil

from buildscripts.resmokelib.testing.fixtures import replicaset, shardedcluster
from buildscripts.resmokelib.testing.hooks import interface


class MongotHook(interface.Hook):
    """Removes data directory files (eg config journals) associated with each mongot launched from a replica set."""

    DESCRIPTION = ("hook for managing and cleaning up data files for mongot")

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture):
        """Initialize the MongotHook.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: ReplicaSetFixture or ShardedClusterFixture with launch_mongot enabled.
        """
        interface.Hook.__init__(self, hook_logger, fixture, MongotHook.DESCRIPTION)
        if not fixture.launch_mongot:
            raise ValueError("The Mongot hook requires launch_mongot to be enabled")

        self.fixture = None
        if isinstance(fixture,
                      (replicaset.ReplicaSetFixture, shardedcluster.ShardedClusterFixture)):
            self.fixture = fixture
        else:
            raise ValueError(
                "The Mongot hook requires a ReplicaSetFixture or ShardedClusterFixture")

    def clear_mongot_data_files(self):
        # Remove each MongoTFixture's associated config journal.
        for mongot in self.fixture._all_mongots():
            self.logger.info("Deleting mongot data dir: %s", mongot.data_dir)
            try:
                shutil.rmtree(mongot.data_dir)
            except OSError as error:
                self.logger.error("Hit OS error trying to delete mongot config journal: %s", error)
                pass

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Begin deleting mongot data files after suite finished")
        self.clear_mongot_data_files()
        self.logger.info("Finished deleting mongot data files after suite finished")

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Begin deleting mongot data files after individual test finished")
        self.clear_mongot_data_files()
        self.logger.info("Finished deleting mongot data files after individual test finished")
