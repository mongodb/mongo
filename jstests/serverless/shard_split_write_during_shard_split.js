/**
 *
 * Tests that runs a shard split to completion and tries to write before and during the split.
 * @tags: [requires_fcv_62, serverless]
 */

import {doWriteOperations, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

const test = new ShardSplitTest();
test.addAndAwaitRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const tenantIds = [ObjectId(), ObjectId()];

jsTestLog("Writing data before split");
tenantIds.forEach(id => {
    const kDbName = test.tenantDB(id.str, "testDb");
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
const writeThread = new Thread(doWriteOperations, donorRst, tojson(tenantIds));
writeThread.start();

blockingFP.off();

splitThread.join();
const result = splitThread.returnData();
assert.eq(result.ok, 1);

writeThread.join();
const writeResults = writeThread.returnData();
writeResults.forEach(res => {
    assert.eq(res, ErrorCodes.TenantMigrationCommitted);
});

TestData.skipCheckDBHashes = true;

test.stop();
