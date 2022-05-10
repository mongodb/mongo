/*
 * Test that tenant access blockers are removed from recipients when applying the recipient config.
 *
 * @tags: [requires_fcv_52, featureFlagShardSplit, serverless]
 */

load("jstests/libs/fail_point_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");

(function() {
"use strict";

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const test = new BasicServerlessTest({
    recipientTagName: "recipientNode",
    recipientSetName: "recipient",
    quickGarbageCollection: true
});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const tenantIds = ["tenant1", "tenant2"];
const operation = test.createSplitOperation(tenantIds);

const donorAfterBlockingFailpoint =
    configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");
test.recipientNodes.forEach(
    node => configureFailPoint(node.getDB("admin"), "skipShardSplitRecipientCleanup"));

const commitOp = operation.commitAsync();
donorAfterBlockingFailpoint.wait();

jsTestLog("Asserting recipient nodes have installed access blockers");
assert.soon(() => test.recipientNodes.every(node => {
    const accessBlockers = BasicServerlessTest.getTenantMigrationAccessBlocker({node});
    return tenantIds.every(tenantId => accessBlockers && accessBlockers.hasOwnProperty(tenantId) &&
                               !!accessBlockers[tenantId].donor);
}));
donorAfterBlockingFailpoint.off();

commitOp.join();
assert.commandWorked(commitOp.returnData());

jsTestLog("Asserting recipient nodes have removed access blockers");
assert.soon(() => test.recipientNodes.every(node => {
    return BasicServerlessTest.getTenantMigrationAccessBlocker({node}) == null;
}));

test.stop();
})();
