/**
 * Utilities for testing tenant migrations.
 */
var TenantMigrationUtil = (function() {
    // An object that mirrors the access states for the TenantMigrationAccessBlocker.
    const accessState = {kAllow: 0, kBlockingWrites: 1, kBlockingReadsAndWrites: 2, kReject: 3};

    /**
     * Runs the donorStartMigration command with the given migration options every 'intervalMS'
     * until the migration commits or aborts, or until the command fails. Returns the last command
     * response.
     */
    function startMigration(donorPrimaryHost, migrationOpts, intervalMS = 100) {
        const donorPrimary = new Mongo(donorPrimaryHost);
        const cmdObj = {
            donorStartMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
            recipientConnectionString: migrationOpts.recipientConnString,
            databasePrefix: migrationOpts.dbPrefix,
            readPreference: migrationOpts.readPreference
        };

        while (true) {
            const res = donorPrimary.adminCommand(cmdObj);
            if (!res.ok || res.state == "committed" || res.state == "aborted") {
                return res;
            }
            sleep(intervalMS);
        }
    }

    /**
     * Runs the donorForgetMigration command with the given migrationId and returns the response.
     */
    function forgetMigration(donorPrimaryHost, migrationIdString) {
        const donorPrimary = new Mongo(donorPrimaryHost);
        return donorPrimary.adminCommand(
            {donorForgetMigration: 1, migrationId: UUID(migrationIdString)});
    }

    return {
        accessState,
        startMigration,
        forgetMigration,
    };
})();
