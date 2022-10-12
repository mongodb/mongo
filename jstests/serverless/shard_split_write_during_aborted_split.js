/**
 *
 * Test that starts a shard split and abort it while doing a write.
 * @tags: [requires_fcv_62, serverless]
 */

load("jstests/serverless/libs/shard_split_test.js");

(function() {
"use strict";

const recipientTagName = "recipientNode";
const recipientSetName = "recipientSetName";
const test = new ShardSplitTest({
    recipientTagName,
    recipientSetName,
    nodeOptions: {
        // Set a short timeout to test that the operation times out waiting for replication
        setParameter: "shardSplitTimeoutMS=100000"
    }
});

test.addRecipientNodes();
test.donor.awaitSecondaryNodes();

const donorPrimary = test.donor.getPrimary();
const tenantIds = ["tenant1", "tenant2"];

jsTestLog("Writing data before split");
tenantIds.forEach(id => {
    const kDbName = test.tenantDB(id, "testDb");
    const kCollName = "testColl";
    const kNs = `${kDbName}.${kCollName}`;

    assert.commandWorked(donorPrimary.getCollection(kNs).insert(
        [{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}], {writeConcern: {w: "majority"}}));
});

const operation = test.createSplitOperation(tenantIds);

const blockingFP = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");

const splitThread = operation.commitAsync();

blockingFP.wait();

const donorRst = createRstArgs(test.donor);
test.removeRecipientsFromRstArgs(donorRst);
const writeThread = new Thread(doWriteOperations, donorRst, tenantIds);
writeThread.start();

operation.abort();

blockingFP.off();

splitThread.join();
const result = splitThread.returnData();
assert.commandFailed(result);

writeThread.join();
const writeResults = writeThread.returnData();
writeResults.forEach(res => {
    assert.eq(res, ErrorCodes.OK);
});

TestData.skipCheckDBHashes = true;

test.stop();
})();
