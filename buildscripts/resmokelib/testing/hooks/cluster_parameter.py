"""Set cluster server parameter hook.

A hook to set the given cluster server parameter on a replica set fixture.
"""

from buildscripts.resmokelib.testing.hooks import interface


class ClusterParameter(interface.Hook):
    IS_BACKGROUND = False

    def __init__(self, hook_logger, rs_fixture, key=None, value=None):
        description = "Sets the specified cluster server parameter."
        interface.Hook.__init__(self, hook_logger, rs_fixture, description)
        self._fixture = rs_fixture
        self._key = key
        self._value = value
        self._original_value = None

    def before_suite(self, test_report):
        """Calls setClusterParameter to set the specified parameter on the fixture before running the suite."""
        client = self._fixture.get_primary().mongo_client()
        self._original_value = client.get_database("admin").command(
            {"getClusterParameter": self._key}
        )["clusterParameters"][0]
        # There are extra parameters in the response that aren't part of the original value so
        # they must be removed.
        del self._original_value["_id"]
        del self._original_value["clusterParameterTime"]
        self.logger.info(f"Original parameter saved: {str(self._original_value)}")
        command_request = {
            "setClusterParameter": {self._key: self._value},
        }
        client.get_database("admin").command(command_request)
        self.logger.info(f"Successfully called setClusterParameter to set {self._key}")

    def after_suite(self, test_report, teardown_flag=None):
        """Restores the original value modified by setClusterParameter."""
        client = self._fixture.get_primary().mongo_client()
        command_request = {
            "setClusterParameter": {self._key: self._original_value},
        }
        client.get_database("admin").command(command_request)
        self.logger.info(
            f"Successfully called setClusterParameter to restor original value of {self._key}"
        )
