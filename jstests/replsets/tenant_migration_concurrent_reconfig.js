/**
 * Tests that reconfigs cause tenant migrations to fail cleanly when issued between
 * donor commands sent to the recipient, and that they succeed in all other cases.
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

function runTest({failPoint, shouldFail = false}) {
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    const tenantId = "testTenantId";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorDB = donorPrimary.getDB(dbName);

    const migrationId = UUID();
    const migrationIdString = extractUUIDFromObject(migrationId);
    const migrationOpts = {
        migrationIdString: migrationIdString,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };

    const hangWhileMigratingFP = configureFailPoint(donorDB, failPoint);

    jsTestLog("Starting migration and waiting for donor to hit the failpoint");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    hangWhileMigratingFP.wait();

    jsTestLog("Performing reconfig on donor set");
    donorRst.add({rsConfig: {priority: 0, votes: 0}});
    donorRst.reInitiate();
    donorRst.awaitSecondaryNodes();
    donorRst.awaitReplication();

    hangWhileMigratingFP.off();

    jsTestLog("Waiting for migration to finish");
    if (shouldFail) {
        TenantMigrationTest.assertAborted(
            tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
            ErrorCodes.ConflictingOperationInProgress);

        tenantMigrationTest.waitForDonorNodesToReachState(
            donorRst.nodes, migrationId, tenantId, TenantMigrationTest.DonorState.kAborted);
    } else {
        TenantMigrationTest.assertCommitted(
            tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

        tenantMigrationTest.waitForDonorNodesToReachState(
            donorRst.nodes, migrationId, tenantId, TenantMigrationTest.DonorState.kCommitted);
    }

    jsTestLog("Forgetting migration");
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.stop();
}

jsTestLog("[1] Testing reconfig between recipientSyncData commands.");
runTest({failPoint: "pauseTenantMigrationBeforeLeavingDataSyncState", shouldFail: true});

jsTestLog("[2] Testing reconfig between final recipientSyncData and recipientForgetMigration.");
runTest({failPoint: "pauseTenantMigrationBeforeLeavingBlockingState", shouldFail: false});

jsTestLog("[3] Testing reconfig after initializing and persisting the state doc.");
runTest({failPoint: "pauseTenantMigrationAfterPersistingInitialDonorStateDoc", shouldFail: false});

jsTestLog("[4] Testing reconfig before fetching keys.");
runTest({failPoint: "pauseTenantMigrationBeforeFetchingKeys", shouldFail: false});
})();
