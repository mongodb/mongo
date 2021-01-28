/**
 * Starts a tenant migration that aborts, either due to the
 * abortTenantMigrationBeforeLeavingBlockingState failpoint or due to receiving donorAbortMigration,
 * and then issues a donorForgetMigration command. Finally, starts a second tenant migration with
 * the same tenantId as the aborted migration, and expects this second migration to go through.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantIdPrefix = "testTenantId";
let testNum = 0;

function makeTenantId() {
    return kTenantIdPrefix + testNum++;
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

(() => {
    const migrationId1 = extractUUIDFromObject(UUID());
    const migrationId2 = extractUUIDFromObject(UUID());
    const tenantId = makeTenantId();

    // Start a migration with the "abortTenantMigrationBeforeLeavingBlockingState" failPoint
    // enabled. The migration will abort as a result, and a status of "kAborted" should be returned.
    jsTestLog(
        "Starting a migration that is expected to abort due to setting abortTenantMigrationBeforeLeavingBlockingState failpoint. migrationId: " +
        migrationId1 + ", tenantId: " + tenantId);
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    const abortRes = assert.commandWorked(
        tenantMigrationTest.runMigration({migrationIdString: migrationId1, tenantId: tenantId},
                                         false /* retryOnRetryableErrors */,
                                         false /* automaticForgetMigration */));
    assert.eq(abortRes.state, TenantMigrationTest.State.kAborted);
    abortFp.off();

    // Forget the aborted migration.
    jsTestLog("Forgetting aborted migration with migrationId: " + migrationId1);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationId1));

    // Try running a new migration with the same tenantId. It should succeed, since the previous
    // migration with the same tenantId was aborted.
    jsTestLog("Attempting to run a new migration with the same tenantId. New migrationId: " +
              migrationId2 + ", tenantId: " + tenantId);
    const commitRes = assert.commandWorked(
        tenantMigrationTest.runMigration({migrationIdString: migrationId2, tenantId: tenantId}));
    assert.eq(commitRes.state, TenantMigrationTest.State.kCommitted);
})();

(() => {
    const migrationId1 = extractUUIDFromObject(UUID());
    const migrationId2 = extractUUIDFromObject(UUID());
    const tenantId = makeTenantId();

    jsTestLog(
        "Starting a migration that is expected to abort in blocking state due to receiving donorAbortMigration. migrationId: " +
        migrationId1 + ", tenantId: " + tenantId);

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(
        tenantMigrationTest.startMigration({migrationIdString: migrationId1, tenantId: tenantId}));

    fp.wait();

    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
    const tryAbortThread = new Thread(TenantMigrationUtil.tryAbortMigrationAsync,
                                      {migrationIdString: migrationId1, tenantId: tenantId},
                                      donorRstArgs,
                                      TenantMigrationUtil.runTenantMigrationCommand);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(donorPrimary.adminCommand(
            {currentOp: true, desc: "tenant donor migration", tenantId: tenantId}));
        return res.inprog[0].receivedCancelation;
    });

    fp.off();

    tryAbortThread.join();
    assert.commandWorked(tryAbortThread.returnData());

    const stateRes = assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(
        {migrationIdString: migrationId1, tenantId: tenantId}));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);

    // Forget the aborted migration.
    jsTestLog("Forgetting aborted migration with migrationId: " + migrationId1);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationId1));

    // Try running a new migration with the same tenantId. It should succeed, since the previous
    // migration with the same tenantId was aborted.
    jsTestLog("Attempting to run a new migration with the same tenantId. New migrationId: " +
              migrationId2 + ", tenantId: " + tenantId);
    const commitRes = assert.commandWorked(
        tenantMigrationTest.runMigration({migrationIdString: migrationId2, tenantId: tenantId}));
    assert.eq(commitRes.state, TenantMigrationTest.State.kCommitted);
})();

tenantMigrationTest.stop();
})();