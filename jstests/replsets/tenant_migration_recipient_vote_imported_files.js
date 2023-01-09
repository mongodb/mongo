/**
 * Tests the recipientVoteImportedFiles command.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    runMigrationAsync
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load('jstests/replsets/rslib.js');  // 'createRstArgs'

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    sharedOptions: {setParameter: {tenantMigrationGarbageCollectionDelayMS: 1 * 1000}}
});

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    quit();
}

const kTenantId = ObjectId();

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
    tenantIds: tojson([kTenantId]),
};

const donorRstArgs = createRstArgs(tenantMigrationTest.getDonorRst());

jsTestLog("Test that recipientVoteImportedFiles fails with no migration started");
voteShouldFail(migrationId);

jsTestLog("Start a migration and pause after cloning");
const fpAfterStartingOplogApplierMigrationRecipientInstance = configureFailPoint(
    recipientPrimary, "fpAfterStartingOplogApplierMigrationRecipientInstance", {action: "hang"});
const migrationThread = new Thread(runMigrationAsync, migrationOpts, donorRstArgs);
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
tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId.str);
jsTestLog("Test that recipientVoteImportedFiles fails after migration is forgotten");
voteShouldFail(migrationId);
tenantMigrationTest.stop();
