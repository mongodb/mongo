"""Test hook for verifying data consistency between the donor and recipient primaries in a tenant migration."""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class CheckTenantMigrationDBHash(jsfile.DataConsistencyHook):
    """Check if the dbhashes match.

    This includes dbhashes for all non-local databases and non-replicated system collections that
    match on the primaries of the donor and recipient
    """

    def __init__(  # pylint: disable=super-init-not-called
            self, hook_logger, fixture, shell_options=None):
        """Initialize CheckTenantMigrationDBHash."""
        description = "Check dbhashes of donor and recipient primaries"
        js_filename = os.path.join("jstests", "hooks", "run_check_tenant_migration_dbhash.js")
        jsfile.JSHook.__init__(  # pylint: disable=non-parent-init-called
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options)
