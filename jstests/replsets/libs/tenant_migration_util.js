/**
 * Utilities for testing tenant migrations.
 */
var TenantMigrationUtil = (function() {
    // An object that mirrors the access states for the TenantMigrationAccessBlocker.
    const accessState = {kAllow: 0, kBlockingWrites: 1, kBlockingReadsAndWrites: 2, kReject: 3};

    function startMigration(donorPrimaryHost, migrationOpts) {
        const donorPrimary = new Mongo(donorPrimaryHost);
        return donorPrimary.adminCommand({
            donorStartMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
            recipientConnectionString: migrationOpts.recipientConnString,
            databasePrefix: migrationOpts.dbPrefix,
            readPreference: migrationOpts.readPreference
        });
    }

    function forgetMigration(donorPrimaryHost, migrationIdString) {
        const migrationId = UUID(migrationIdString);
        const donorPrimary = new Mongo(donorPrimaryHost);
        while (true) {
            let res =
                donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId});
            if (res.ok || res.code != ErrorCodes.NoSuchTenantMigration) {
                return res;
            }
        }
    }

    return {
        accessState,
        startMigration,
        forgetMigration,
    };
})();
