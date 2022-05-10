/**
 * Tests that forgetShardSplit command doesn't hang if failover occurs immediately after the
 * state doc for the split has been removed.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_52,
 *   featureFlagShardSplit
 * ]
 */

(function() {
"use strict";

load("jstests/libs/parallelTester.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");

const test = new BasicServerlessTest({
    recipientTagName: "recipientTag",
    recipientSetName: "recipientSet",
    quickGarbageCollection: true
});
test.addRecipientNodes();

const tenantIds = ["testTenantId"];

let donorPrimary = test.donor.getPrimary();

const operation = test.createSplitOperation(tenantIds);
assert.commandWorked(operation.commit());

let fp = configureFailPoint(donorPrimary, "pauseShardSplitAfterMarkingStateGarbageCollectable");

// Remove the recipient nodes as they have left the replica set.
test.removeAndStopRecipientNodes();

const forgetMigrationThread = operation.forgetAsync();

fp.wait();

test.waitForGarbageCollection(operation.migrationId, tenantIds);

// Force a stepdown on the primary.
assert.commandWorked(
    donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
fp.off();

donorPrimary = test.donor.getPrimary();

// Verify forget does not hang and return the expected error code.
forgetMigrationThread.join();
assert.commandFailedWithCode(forgetMigrationThread.returnData(),
                             ErrorCodes.InterruptedDueToReplStateChange);

test.stop();
})();
