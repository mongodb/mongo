/**
 * Tests that donorForgetMigration command doesn't hang if failover occurs immediately after the
 * state doc for the migration has been removed.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/parallelTester.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    sharedOptions: {
        setParameter: {
            tenantMigrationGarbageCollectionDelayMS: 1,
            ttlMonitorSleepSecs: 1,
        }
    },
    initiateRstWithHighElectionTimeout: false
});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kTenantId = "testTenantId";

const donorRst = tenantMigrationTest.getDonorRst();
const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
let donorPrimary = tenantMigrationTest.getDonorPrimary();

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(
    migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));

let fp = configureFailPoint(donorPrimary,
                            "pauseTenantMigrationDonorAfterMarkingStateGarbageCollectable");
const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                         migrationOpts.migrationIdString,
                                         donorRstArgs,
                                         false /* retryOnRetryableErrors */);
forgetMigrationThread.start();
fp.wait();
tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, migrationOpts.tenantId);

assert.commandWorked(
    donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
fp.off();
donorPrimary = donorRst.getPrimary();

assert.commandFailedWithCode(forgetMigrationThread.returnData(),
                             ErrorCodes.InterruptedDueToReplStateChange);

donorRst.stopSet();
tenantMigrationTest.stop();
})();