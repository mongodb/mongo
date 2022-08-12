"""Enable change stream hook.

A hook to enable change stream in the replica set and the sharded cluster in the multi-tenant
environment.
"""
from time import sleep
from pymongo import MongoClient

from buildscripts.resmokelib import config
from buildscripts.resmokelib.testing.hooks import interface


class EnableChangeStream(interface.Hook):
    """Enable change stream hook class.

    Enables change stream in the multi-tenant environment for the replica set and the sharded
    cluster.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture):
        """Initialize the EnableChangeCollection."""
        description = "Enables the change stream in the multi-tenant environment."
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self._fixture = fixture

    def before_test(self, test, test_report):
        """Enables change stream before test suite starts executing the test cases."""
        if hasattr(self._fixture, "mongos"):
            self.logger.info("Enabling change stream in the sharded cluster.")
            self._set_change_collection_state_in_sharded_cluster()
        else:
            self.logger.info("Enabling change stream in the replica sets.")
            self._set_change_stream_state(self._fixture, True)

        self.logger.info("Successfully enabled the change stream in the fixture.")

    def _set_change_collection_state_in_sharded_cluster(self):
        for shard in self._fixture.shards:
            EnableChangeStream._set_change_stream_state(shard, True)

        # TODO SERVER-68341 Remove the sleep. Sleep for some time such that periodic-noop entries
        # get written to change collections. This will ensure that the client open the change
        # stream cursor with the resume token whose timestamp is later than the change collection
        # first entry. Refer to the ticket for more details.
        sleep(5)

    @staticmethod
    def _set_change_stream_state(connection, enabled):
        # TODO SERVER-65950 create change collection for the tenant.
        client = connection.get_primary().mongo_client()
        client.get_database("admin").command({"setChangeStreamState": 1, "enabled": enabled})

        assert client.get_database("admin").command({"getChangeStreamState": 1
                                                     })["enabled"] is enabled
