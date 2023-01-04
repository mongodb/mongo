/*
 * Prove that shard splits are eagerly aborted when the `setFeatureCompatibilityVersion` command is
 * received for both upgrade and downgrade paths.
 *
 * @tags: [requires_fcv_62, serverless]
 */

import {ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/fail_point_util.js");

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;
const test = new ShardSplitTest({quickGarbageCollection: true});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const tenantIds = [ObjectId(), ObjectId()];
const pauseAfterBlockingFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

jsTestLog("Test FCV Downgrade");
const split = test.createSplitOperation(tenantIds);
const commitThread = split.commitAsync();
pauseAfterBlockingFp.wait();
assert.commandWorked(
    donorPrimary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
pauseAfterBlockingFp.off();
assert.commandFailedWithCode(commitThread.returnData(), ErrorCodes.TenantMigrationAborted);

jsTestLog("Test FCV Upgrade");
if (lastContinuousFCV == "6.1") {
    const secondSplit = test.createSplitOperation(tenantIds);
    assert.commandFailedWithCode(secondSplit.commit(), ErrorCodes.IllegalOperation);
} else {
    // `forgetShardSplit` will not be available until the downgraded version also supports the
    // 'shard split' feature.
    split.forget();
    test.cleanupSuccesfulAborted(split.migrationId, tenantIds);

    test.addRecipientNodes();
    const pauseAfterBlockingFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");
    const secondSplit = test.createSplitOperation(tenantIds);
    const commitThread = secondSplit.commitAsync();
    pauseAfterBlockingFp.wait();
    assert.commandWorked(donorPrimary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    pauseAfterBlockingFp.off();
    assert.commandFailedWithCode(commitThread.returnData(), ErrorCodes.TenantMigrationAborted);
    secondSplit.forget();
    test.cleanupSuccesfulAborted(secondSplit.migrationId, tenantIds);
}

test.stop();
