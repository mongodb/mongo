/**
 * Utilities for testing tenant migrations.
 */
var TenantMigrationUtil = (function() {
    load("jstests/replsets/libs/tenant_migration_test.js");

    /**
     * Returns whether tenant migration commands are supported.
     */
    function isFeatureFlagEnabled(conn) {
        return assert
            .commandWorked(conn.adminCommand({getParameter: 1, featureFlagTenantMigrations: 1}))
            .featureFlagTenantMigrations.value;
    }

    /**
     * Runs the donorStartMigration command with the given migration options
     * until the migration commits or aborts, or until the command fails. Returns the last command
     * response.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails with a
     * NotPrimary or network error.
     *
     * Only use when it is necessary to run the donorStartMigration command in its own thread. For
     * all other use cases, please consider the runMigration() function in the TenantMigrationTest
     * fixture.
     */
    function runMigrationAsync(migrationOpts, donorRstArgs, retryOnRetryableErrors = false) {
        const cmdObj = {
            donorStartMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
            recipientConnectionString: migrationOpts.recipientConnString,
            tenantId: migrationOpts.tenantId,
            readPreference: migrationOpts.readPreference || {mode: "primary"},
        };

        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
        let donorPrimary = donorRst.getPrimary();

        let res;
        assert.soon(() => {
            try {
                res = donorPrimary.adminCommand(cmdObj);

                if (!res.ok) {
                    // If retry is enabled and the command failed with a NotPrimary error, continue
                    // looping.
                    if (retryOnRetryableErrors && ErrorCodes.isNotPrimaryError(res.code)) {
                        donorPrimary = donorRst.getPrimary();
                        return false;
                    }
                    return true;
                }

                return (res.state === "committed" || res.state === "aborted");
            } catch (e) {
                // If the thrown error is network related and we are allowing retryable errors,
                // continue issuing commands.
                if (retryOnRetryableErrors && isNetworkError(e)) {
                    return false;
                }
                throw e;
            }
        });
        return res;
    }

    /**
     * Runs the donorForgetMigration command with the given migrationId and returns the response.
     *
     * If 'retryOnRetryableErrors' is set, this function will retry if the command fails with a
     * NotPrimary or network error.
     *
     * Only use when it is necessary to run the donorForgetMigration command in its own thread. For
     * all other use cases, please consider the forgetMigration() function in the
     * TenantMigrationTest fixture.
     */
    function forgetMigrationAsync(migrationIdString, donorRstArgs, retryOnRetryableErrors = false) {
        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
        let donorPrimary = donorRst.getPrimary();

        let res;

        assert.soon(() => {
            try {
                res = donorPrimary.adminCommand(
                    {donorForgetMigration: 1, migrationId: UUID(migrationIdString)});

                if (!res.ok) {
                    // If retry is enabled and the command failed with a NotPrimary error, continue
                    // looping.
                    if (retryOnRetryableErrors && ErrorCodes.isNotPrimaryError(res.code)) {
                        donorPrimary = donorRst.getPrimary();
                        return false;
                    }
                }

                return true;
            } catch (e) {
                if (retryOnRetryableErrors && isNetworkError(e)) {
                    return false;
                }
                throw e;
            }
        });
        return res;
    }

    function createRstArgs(donorRst) {
        const donorRstArgs = {
            name: donorRst.name,
            nodeHosts: donorRst.nodes.map(node => `127.0.0.1:${node.port}`),
            nodeOptions: donorRst.nodeOptions,
            keyFile: donorRst.keyFile,
            host: donorRst.host,
            waitForKeys: false,
        };
        return donorRstArgs;
    }

    return {runMigrationAsync, forgetMigrationAsync, createRstArgs, isFeatureFlagEnabled};
})();
