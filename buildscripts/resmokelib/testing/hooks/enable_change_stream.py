"""Enable change stream hook.

A hook to enable change stream in the replica set and the sharded cluster in the multi-tenant
environment.
"""

import os.path
from time import sleep

from bson.objectid import ObjectId

from buildscripts.resmokelib.testing.hooks import interface, jsfile


class EnableChangeStream(interface.Hook):
    """Enable change stream hook class.

    Enables change stream in the multi-tenant environment for the replica set and the sharded
    cluster.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, tenant_id=None):
        """Initialize the EnableChangeCollection."""
        description = "Enables the change stream in the multi-tenant environment."
        self._js_filename = os.path.join("jstests", "hooks", "run_enable_change_stream.js")
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self._fixture = fixture
        self._tenant_id = ObjectId(tenant_id) if tenant_id else None

    def before_test(self, test, test_report):
        """Enables change stream before test suite starts executing the test cases."""
        if hasattr(self._fixture, "mongos"):
            self.logger.info("Enabling change stream in the sharded cluster.")
            self._set_change_collection_state_in_sharded_cluster(test, test_report)
        else:
            self.logger.info("Enabling change stream in the replica sets.")
            self._call_js_hook(self._fixture, test, test_report)

    def _set_change_collection_state_in_sharded_cluster(self, test, test_report):
        for shard in self._fixture.shards:
            self._call_js_hook(shard, test, test_report)
        # TODO SERVER-68341 Remove the sleep. Sleep for some time such that periodic-noop entries
        # get written to change collections. This will ensure that the client open the change
        # stream cursor with the resume token whose timestamp is later than the change collection
        # first entry. Refer to the ticket for more details.
        sleep(5)

    def _call_js_hook(self, fixture, test, test_report):
        shell_options = {"global_vars": {"TestData": {"tenantId": str(self._tenant_id)}}}
        hook_test_case = jsfile.DynamicJSTestCase.create_before_test(
            test.logger, test, self, self._js_filename, shell_options
        )
        hook_test_case.configure(fixture)
        hook_test_case.run_dynamic_test(test_report)
