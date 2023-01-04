/*
 * Test that tenant access blockers are removed when applying the recipient config
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
const operation = test.createSplitOperation(tenantIds);

const donorAfterBlockingFailpoint =
    configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");
test.recipientNodes.forEach(
    node => configureFailPoint(node.getDB("admin"), "skipShardSplitRecipientCleanup"));

const commitOp = operation.commitAsync();
donorAfterBlockingFailpoint.wait();

jsTestLog("Asserting recipient nodes have installed access blockers");
assert.soon(() => test.recipientNodes.every(node => {
    return tenantIds.every(tenantId => {
        const accessBlocker = ShardSplitTest.getTenantMigrationAccessBlocker({node, tenantId});
        return accessBlocker && accessBlocker.hasOwnProperty(tenantId.str) &&
            !!accessBlocker[tenantId.str].donor;
    });
}));
donorAfterBlockingFailpoint.off();

commitOp.join();
assert.commandWorked(commitOp.returnData());

jsTestLog("Asserting recipient nodes have removed access blockers");
assert.soon(() => test.recipientNodes.every(node => {
    return ShardSplitTest.getTenantMigrationAccessBlocker({node}) == null;
}));

test.stop();
