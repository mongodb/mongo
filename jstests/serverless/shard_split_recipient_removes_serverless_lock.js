/*
 * Test the serverless operation lock is released from recipients when the state document is
 * removed.
 *
 * @tags: [requires_fcv_62, serverless]
 */

import {
    getServerlessOperationLock,
    ServerlessLockType
} from "jstests/replsets/libs/tenant_migration_util.js";
import {ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/fail_point_util.js");

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const test = new ShardSplitTest({quickGarbageCollection: true});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const tenantIds = [ObjectId(), ObjectId()];
const operation = test.createSplitOperation(tenantIds);

const donorAfterBlockingFailpoint =
    configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");

const commitOp = operation.commitAsync();
donorAfterBlockingFailpoint.wait();

jsTestLog("Asserting recipient nodes have installed the serverless lock");
assert.soon(() => test.recipientNodes.every(node => getServerlessOperationLock(node) ===
                                                ServerlessLockType.ShardSplitDonor));
donorAfterBlockingFailpoint.off();

commitOp.join();
assert.commandWorked(commitOp.returnData());

jsTestLog("Asserting the serverless exclusion lock has been released");
assert.soon(() => test.recipientNodes.every(node => getServerlessOperationLock(node) ==
                                                ServerlessLockType.None));

test.stop();
