/**
 * Tests that forgetShardSplit command doesn't hang if failover occurs while it is being processed.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_62
 * ]
 */

(function() {
"use strict";

load("jstests/libs/parallelTester.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/serverless/libs/shard_split_test.js");

const test = new ShardSplitTest({
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

// Wait until `forgetShardSplit` has been received to trigger the stepdown.
fp.wait();

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
