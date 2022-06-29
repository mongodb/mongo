/**
 *
 * Tests that we can't run concurrent shard splits.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');  // for "startParallelShell"
load("jstests/serverless/libs/basic_serverless_test.js");

(function() {
"use strict";

const recipientTagName = "recipientNode";
const recipientSetName = "recipientSetName";
const tenantIds = ["tenant1", "tenant2"];

function commitShardSplitConcurrently() {
    const test =
        new BasicServerlessTest({recipientTagName, recipientSetName, quickGarbageCollection: true});
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();

    let fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");
    let fpAfterDecision =
        configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterDecision");

    const operation = test.createSplitOperation(tenantIds);
    const splitThread = operation.commitAsync();

    fp.wait();

    // fails because there is an ongoing shard split in blocking state.
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: ["tenant3", "tenant4"],
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress

    fp.off();

    // blocks before processing any `forgetShardSplit` command.
    fpAfterDecision.wait();
    let forgetThread = operation.forgetAsync();

    // fails because the commitShardSplit hasn't be garbage collected yet.
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: ["tenant1", "tenant3"],  // re use one tenantid
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress
    fpAfterDecision.off();
    assert.commandWorked(splitThread.returnData());
    forgetThread.join();

    test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);

    // another split operation can start after garbage collection of the previous one.
    test.addRecipientNodes();
    assert.commandWorked(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: ["tenant10", "tenant11"],
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }));

    test.stop();
}

commitShardSplitConcurrently();
})();
