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
        # TODO SERVER-67634 avoid using change collection in the config server.
        EnableChangeStream._set_change_stream_state(self._fixture.configsvr, True)

        for shard in self._fixture.shards:
            EnableChangeStream._set_change_stream_state(shard, True)

        # TODO SERVER-68341 remove the sleep. Refer to the ticket for details.
        sleep(5)

    @staticmethod
    def _set_change_stream_state(connection, enabled):
        # TODO SERVER-65950 create change collection for the tenant.
        client = connection.get_primary().mongo_client()
        client.get_database("admin").command({"setChangeStreamState": 1, "enabled": enabled})

        assert client.get_database("admin").command({"getChangeStreamState": 1
                                                     })["enabled"] is enabled
