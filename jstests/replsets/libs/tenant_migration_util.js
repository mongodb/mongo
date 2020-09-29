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
        const cmdObj = {
            donorStartMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
            recipientConnectionString: migrationOpts.recipientConnString,
            tenantId: migrationOpts.tenantId,
            readPreference: migrationOpts.readPreference
        };
        const donorPrimary = new Mongo(donorPrimaryHost);

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
     * Runs the donorStartMigration command with the given migration options every 'intervalMS'
     * until the migration commits or aborts, or until the command fails with error other than
     * NotPrimary or network errors. Returns the last command response.
     */
    function startMigrationRetryOnRetryableErrors(donorRstArgs, migrationOpts, intervalMS = 100) {
        const cmdObj = {
            donorStartMigration: 1,
            migrationId: UUID(migrationOpts.migrationIdString),
            recipientConnectionString: migrationOpts.recipientConnString,
            tenantId: migrationOpts.tenantId,
            readPreference: migrationOpts.readPreference
        };

        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
        let donorPrimary = donorRst.getPrimary();

        while (true) {
            let res;
            try {
                res = donorPrimary.adminCommand(cmdObj);
            } catch (e) {
                if (isNetworkError(e)) {
                    jsTest.log(`Ignoring network error ${tojson(e)} for command ${tojson(cmdObj)}`);
                    continue;
                }
                throw e;
            }

            if (!res.ok) {
                if (!ErrorCodes.isNotPrimaryError(res.code)) {
                    return res;
                }
                donorPrimary = donorRst.getPrimary();
            } else if (res.state == "committed" || res.state == "aborted") {
                return res;
            }
            sleep(intervalMS);
        }
    }

    /**
     * Runs the donorForgetMigration command with the given migrationId until the command succeeds
     * or fails with error other than NotPrimary or network errors. Returns the last command
     * response.
     */
    function forgetMigrationRetryOnRetryableErrors(donorRstArgs, migrationIdString) {
        const cmdObj = {donorForgetMigration: 1, migrationId: UUID(migrationIdString)};

        const donorRst = new ReplSetTest({rstArgs: donorRstArgs});
        let donorPrimary = donorRst.getPrimary();

        while (true) {
            let res;
            try {
                res = donorPrimary.adminCommand(cmdObj);
            } catch (e) {
                if (isNetworkError(e)) {
                    jsTest.log(`Ignoring network error ${tojson(e)} for command ${tojson(cmdObj)}`);
                    continue;
                }
                throw e;
            }

            if (res.ok || !ErrorCodes.isNotPrimaryError(res.code)) {
                return res;
            }
            donorPrimary = donorRst.getPrimary();
        }
    }

    /**
     * Returns true if the durable and in-memory state for the migration 'migrationId' and
     * 'tenantId' is in state "committed", and false otherwise.
     */
    function isMigrationCommitted(node, migrationId, tenantId) {
        const configDonorsColl = node.getCollection("config.tenantMigrationDonors");
        if (configDonorsColl.findOne({_id: migrationId}).state != "committed") {
            return false;
        }
        const mtabs = node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtabs[tenantId].access === TenantMigrationUtil.accessState.kReject;
    }

    /**
     * Returns true if the durable and in-memory state for the migration 'migrationId' and
     * 'tenantId' is in state "aborted", and false otherwise.
     */
    function isMigrationAborted(node, migrationId, tenantId) {
        const configDonorsColl = node.getCollection("config.tenantMigrationDonors");
        if (configDonorsColl.findOne({_id: migrationId}).state != "aborted") {
            return false;
        }
        const mtabs = node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtabs[tenantId].access === TenantMigrationUtil.accessState.kAllow;
    }

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' is in state "committed" on all the
     * given nodes.
     */
    function assertMigrationCommitted(nodes, migrationId, tenantId) {
        nodes.forEach(node => {
            assert(isMigrationCommitted(node, migrationId, tenantId));
        });
    }

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' eventually goes to state "committed"
     * on all the given nodes.
     */
    function waitForMigrationToCommit(nodes, migrationId, tenantId) {
        nodes.forEach(node => {
            assert.soon(() => isMigrationCommitted(node, migrationId, tenantId));
        });
    }

    /**
     * Asserts that the migration 'migrationId' and 'tenantId' eventually goes to state "aborted"
     * on all the given nodes.
     */
    function waitForMigrationToAbort(nodes, migrationId, tenantId) {
        nodes.forEach(node => {
            assert.soon(() => isMigrationAborted(node, migrationId, tenantId));
        });
    }

    /**
     * Asserts that durable and in-memory state for the migration 'migrationId' and 'tenantId' is
     * eventually deleted from the given nodes.
     */
    function waitForMigrationGarbageCollection(nodes, migrationId, tenantId) {
        nodes.forEach(node => {
            const configDonorsColl = node.getCollection("config.tenantMigrationDonors");
            assert.soon(() => 0 === configDonorsColl.count({_id: migrationId}));

            assert.soon(() => 0 ===
                            node.adminCommand({serverStatus: 1})
                                .repl.primaryOnlyServices.TenantMigrationDonorService);

            let mtabs;
            assert.soon(() => {
                mtabs = node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
                return !mtabs || !mtabs[tenantId];
            }, tojson(mtabs));
        });
    }

    /**
     * Returns the TenantMigrationAccessBlocker associated with given the tenantId on the
     * node.
     */
    function getTenantMigrationAccessBlocker(node, tenantId) {
        return node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker[tenantId];
    }

    /**
     * Crafts a tenant database name, given the tenantId.
     */
    function tenantDB(tenantId, dbName) {
        return `${tenantId}_${dbName}`;
    }

    /**
     * Crafts a database name that does not belong to the tenant, given the tenantId.
     */
    function nonTenantDB(tenantId, dbName) {
        return `non_${tenantId}_${dbName}`;
    }

    return {
        accessState,
        startMigration,
        forgetMigration,
        startMigrationRetryOnRetryableErrors,
        forgetMigrationRetryOnRetryableErrors,
        assertMigrationCommitted,
        waitForMigrationToCommit,
        waitForMigrationToAbort,
        waitForMigrationGarbageCollection,
        getTenantMigrationAccessBlocker,
        tenantDB,
        nonTenantDB,
    };
})();
