/**
 *
 * Tests that we can't run concurrent shard splits.
 * @tags: [requires_fcv_62, serverless]
 */

import {ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/fail_point_util.js");  // for "configureFailPoint"

const tenantIds = [ObjectId(), ObjectId()];

function commitShardSplitConcurrently() {
    const test = new ShardSplitTest({quickGarbageCollection: true});
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();

    const fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");
    const fpAfterDecision =
        configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterDecision");

    const operation = test.createSplitOperation(tenantIds);
    const splitThread = operation.commitAsync();

    fp.wait();

    // fails because there is an ongoing shard split in blocking state.
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: [ObjectId(), ObjectId()],
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress

    fp.off();

    // blocks before processing any `forgetShardSplit` command.
    fpAfterDecision.wait();
    const forgetThread = operation.forgetAsync();

    // fails because the commitShardSplit hasn't be garbage collected yet.
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: [ObjectId(), ObjectId()],  // re use one tenantid
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }),
                                 117);  // ConflictingOperationInProgress
    fpAfterDecision.off();
    assert.commandWorked(splitThread.returnData());
    assert.commandWorked(forgetThread.returnData());

    test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);

    // another split operation can start after garbage collection of the previous one.
    test.addRecipientNodes();
    assert.commandWorked(donorPrimary.adminCommand({
        commitShardSplit: 1,
        migrationId: UUID(),
        tenantIds: [ObjectId(), ObjectId()],
        recipientTagName: test.recipientTagName,
        recipientSetName: test.recipientSetName
    }));

    test.stop();
}

commitShardSplitConcurrently();
