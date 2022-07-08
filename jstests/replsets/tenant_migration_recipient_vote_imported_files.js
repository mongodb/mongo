/**
 * Tests the recipientVoteImportedFiles command.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_fcv_52,
 *   serverless,
 *   featureFlagShardMerge,
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    sharedOptions: {setParameter: {tenantMigrationGarbageCollectionDelayMS: 1 * 1000}}
});

const kTenantId = "testTenantId1";
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

function runVoteCmd(migrationId) {
    // Pretend the primary tells itself it has imported files. This may preempt the primary's real
    // life message, but that's ok. We use a failpoint to prevent migration from progressing too
    // far.
    return recipientPrimary.adminCommand({
        recipientVoteImportedFiles: 1,
        migrationId: migrationId,
        from: tenantMigrationTest.getRecipientPrimary().host,
        success: true
    });
}

function voteShouldFail(migrationId) {
    const reply = runVoteCmd(migrationId);
    jsTestLog(`Vote with migrationId ${migrationId}, reply` +
              ` (should fail): ${tojson(reply)}`);
    assert.commandFailed(reply);
}

function voteShouldSucceed(migrationId) {
    assert.commandWorked(runVoteCmd(migrationId));
}

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenantId,
};

const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

jsTestLog("Test that recipientVoteImportedFiles fails with no migration started");
voteShouldFail(migrationId);

jsTestLog("Start a migration and pause after cloning");
const fpAfterStartingOplogApplierMigrationRecipientInstance = configureFailPoint(
    recipientPrimary, "fpAfterStartingOplogApplierMigrationRecipientInstance", {action: "hang"});
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();

jsTestLog("Wait for recipient to log 'Waiting for all nodes to call recipientVoteImportedFiles'");
assert.soon(() => checkLog.checkContainsOnceJson(recipientPrimary, 6113402, {}));

jsTestLog("Test that recipientVoteImportedFiles succeeds");
voteShouldSucceed(migrationId);

jsTestLog("Test that recipientVoteImportedFiles fails with wrong migrationId");
voteShouldFail(UUID());

fpAfterStartingOplogApplierMigrationRecipientInstance.wait();
fpAfterStartingOplogApplierMigrationRecipientInstance.off();

TenantMigrationTest.assertCommitted(migrationThread.returnData());
jsTestLog("Test that recipientVoteImportedFiles succeeds after migration commits");
// Just a delayed message, the primary replies "ok".
voteShouldSucceed(migrationId);
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
jsTestLog("Await garbage collection");
tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId);
jsTestLog("Test that recipientVoteImportedFiles fails after migration is forgotten");
voteShouldFail(migrationId);
tenantMigrationTest.stop();
})();
