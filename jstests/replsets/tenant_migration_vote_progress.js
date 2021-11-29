/**
 * Tests the voteCommitMigrationProgress command.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_fcv_52,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kTenantId = "testTenantId1";
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

function runVoteCmd(migrationId, step) {
    return recipientPrimary.adminCommand({
        voteCommitMigrationProgress: 1,
        migrationId: migrationId,
        from: tenantMigrationTest.getRecipientPrimary().host,
        step: step,
        success: true
    });
}

function voteShouldFail(migrationId, steps) {
    for (let step of steps) {
        const reply = runVoteCmd(migrationId, step);
        jsTestLog(`Vote with migrationId ${migrationId}, step '${step}', reply` +
                  ` (should fail): ${tojson(reply)}`);
        assert.commandFailed(reply);
    }
}

function voteShouldSucceed(migrationId, steps) {
    for (let step of steps) {
        assert.commandWorked(runVoteCmd(migrationId, step));
    }
}

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenantId,
};

const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

jsTestLog("Test that voteCommitMigrationProgress fails with no migration in flight");
voteShouldFail(migrationId, ["copied files", "imported files"]);

jsTestLog("Start a migration and pause after cloning");
const fpAfterCollectionClonerDone =
    configureFailPoint(recipientPrimary, "fpAfterCollectionClonerDone", {action: "hang"});
const fpAfterDataConsistentMigrationRecipientInstance = configureFailPoint(
    recipientPrimary, "fpAfterDataConsistentMigrationRecipientInstance", {action: "hang"});
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
fpAfterCollectionClonerDone.wait();
fpAfterCollectionClonerDone.off();

if (TenantMigrationUtil.isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    jsTestLog("Test that voteCommitMigrationProgress succeeds with step 'copied files'");
    voteShouldSucceed(migrationId, ["copied files"]);
} else {
    jsTestLog("Test that voteCommitMigrationProgress fails with shard merge disabled");
    voteShouldFail(migrationId, ["copied files"]);
}

jsTestLog("Test that voteCommitMigrationProgress fails with wrong 'step'");
voteShouldFail(migrationId, ["imported files"]);

fpAfterDataConsistentMigrationRecipientInstance.wait();
fpAfterDataConsistentMigrationRecipientInstance.off();

TenantMigrationTest.assertCommitted(migrationThread.returnData());
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
voteShouldFail(migrationId, ["copied files", "imported files"]);
tenantMigrationTest.stop();
})();
