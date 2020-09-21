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

    /**
     * Returns true if the durable and in-memory state for the migration 'migrationId' and
     * 'dbPrefix' is in state "committed", and false otherwise.
     */
    function isMigrationCommitted(node, migrationId, dbPrefix) {
        const configDonorsColl = node.getCollection("config.tenantMigrationDonors");
        if (configDonorsColl.findOne({_id: migrationId}).state != "committed") {
            return false;
        }
        const mtabs = node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtabs[dbPrefix].access === TenantMigrationUtil.accessState.kReject;
    }

    /**
     * Asserts that the migration 'migrationId' and 'dbPrefix' is in state "committed" on all the
     * given nodes.
     */
    function assertMigrationCommitted(nodes, migrationId, dbPrefix) {
        nodes.forEach(node => {
            assert(isMigrationCommitted(node, migrationId, dbPrefix));
        });
    }

    /**
     * Asserts that the migration 'migrationId' and 'dbPrefix' eventually goes to state "committed"
     * on all the given nodes.
     */
    function waitForMigrationToCommit(nodes, migrationId, dbPrefix) {
        nodes.forEach(node => {
            assert.soon(() => isMigrationCommitted(node, migrationId, dbPrefix));
        });
    }

    /**
     * Asserts that durable and in-memory state for the migration 'migrationId' and 'dbPrefix' is
     * eventually deleted from the given nodes.
     */
    function waitForMigrationGarbageCollection(nodes, migrationId, dbPrefix) {
        nodes.forEach(node => {
            const configDonorsColl = node.getCollection("config.tenantMigrationDonors");
            assert.soon(() => 0 === configDonorsColl.count({_id: migrationId}));

            assert.soon(() => 0 ===
                            node.adminCommand({serverStatus: 1})
                                .repl.primaryOnlyServices.TenantMigrationDonorService);

            let mtabs;
            assert.soon(() => {
                mtabs = node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
                return !mtabs || !mtabs[dbPrefix];
            }, tojson(mtabs));
        });
    }

    return {
        accessState,
        startMigration,
        forgetMigration,
        assertMigrationCommitted,
        waitForMigrationToCommit,
        waitForMigrationGarbageCollection
    };
})();
