"""Test hook that unlocks any fsync-locked mongod fixtures after a test."""

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture
from buildscripts.resmokelib.testing.hooks import interface


class FsyncUnlockAfterTest(interface.Hook):
    """
    Unlock the fsync-locked fixture after the test.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture):
        interface.Hook.__init__(self, hook_logger, fixture, "Unlock fsync-locked fixture")

    def _is_fsync_locked(self, client):
        """
        Check if the given client is fsync-locked
        """
        try:
            res = client.admin.command({"currentOp": 1})
            return bool(res.get("fsyncLock", False))
        except Exception:
            self.logger.exception("Failed to check fsync state")
            raise errors.TestFailure("Failed to check fsync state")

    def _is_supported_node(self, node):
        """
        Check if the given node fixture is supported for this hook.
        """
        return isinstance(node, MongoDFixture)

    def _unlock_if_needed(self, node):
        """
        Unlock the given node if it is fsync-locked.
        Returns True if the node was unlocked, False otherwise.
        """
        if not self._is_supported_node(node):
            return False
        client = node.mongo_client()
        try:
            if not self._is_fsync_locked(client):
                return False
            # Unlock the fsync-locked fixture
            self.logger.warning("fsyncLock detected, unlocking...")
            # unlock to get the remaining lock count
            res = client.admin.command({"fsyncUnlock": 1})
            lock_count = res.get("lockCount", 0)

            # Loop through the remaining lock count and unlock one at a time
            for _ in range(lock_count):
                res = client.admin.command({"fsyncUnlock": 1})
                self.logger.info("fsyncUnlock remaining lock count: %s", res.get("lockCount", 0))

            # final unlock check
            if self._is_fsync_locked(client):
                self.logger.error("Failed to unlock fsync-locked fixture")
                raise errors.TestFailure("Failed to unlock fsync-locked fixture")
            return True
        finally:
            client.close()

    def after_test(self, test, test_report):
        """
        Unlock the fsync-locked fixture after the test.
        """
        self.logger.info("Unlocking fsync-locked fixture")

        # Count the number of fsync-locked nodes
        nodes_locked = 0
        for cluster in self.fixture.get_independent_clusters():
            for node in cluster._all_mongo_d_s_t():
                # Only count mongod fixtures
                if not self._is_supported_node(node):
                    continue
                client = node.mongo_client()
                try:
                    if self._is_fsync_locked(client):
                        nodes_locked += 1
                finally:
                    client.close()

        if nodes_locked == 0:
            self.logger.info("No fsync-locked nodes found")
            return

        # unlock the fsync-locked nodes
        self.logger.info("Unlocking %s fsync-locked nodes", nodes_locked)
        nodes_unlocked = 0
        for cluster in self.fixture.get_independent_clusters():
            for node in cluster._all_mongo_d_s_t():
                if self._unlock_if_needed(node):
                    nodes_unlocked += 1
        if nodes_unlocked != nodes_locked:
            self.logger.error(
                "Failed to unlock %s fsync-locked nodes", nodes_locked - nodes_unlocked
            )
            raise errors.TestFailure(
                f"Failed to unlock {nodes_locked - nodes_unlocked} fsync-locked nodes"
            )
        else:
            self.logger.info("Successfully unlocked %s fsync-locked nodes", nodes_unlocked)
