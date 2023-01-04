/*
 * Test that a well timed abortShardSplit command does not abort an already committed split.
 *
 * @tags: [requires_fcv_62, serverless]
 */

import {assertMigrationState, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/fail_point_util.js");

const failpoints = ["pauseShardSplitAfterUpdatingToCommittedState"];

function testAbortAfterSplitIsAppliedStillsCommits(failpoint) {
    "use strict";

    const tenantIds = [ObjectId(), ObjectId()];

    const test = new ShardSplitTest({quickGarbageCollection: true});
    test.addRecipientNodes();

    const donorPrimary = test.getDonorPrimary();
    const operation = test.createSplitOperation(tenantIds);

    let blockFp = configureFailPoint(donorPrimary, failpoint);
    let splitThread = operation.commitAsync();
    blockFp.wait();

    // abortCmd expects to have the decisionPromise fullfilled which would be blocked by the
    // `failpoint` already blocking from returning the promise.
    let ranAbortFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterReceivingAbortCmd");
    let abortThread = operation.abortAsync();
    ranAbortFp.wait();

    blockFp.off();

    assert.commandWorked(splitThread.returnData());

    // now that the decisionPromise is fullfilled we can remove the failpoint.
    ranAbortFp.off();
    assert.commandFailed(abortThread.returnData());
    assertMigrationState(donorPrimary, operation.migrationId, "committed");

    operation.forget();
    test.waitForGarbageCollection(operation.migrationId, tenantIds);
    test.stop();
}

failpoints.forEach(fp => {
    testAbortAfterSplitIsAppliedStillsCommits(fp);
});
