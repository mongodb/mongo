/**
 * Test that starts a shard split and abort it while doing a write.
 * @tags: [requires_fcv_63, serverless]
 */

import {ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

TestData.skipCheckDBHashes = true;
const test = new ShardSplitTest({
    nodeOptions: {
        // Set a short timeout to test that the operation times out waiting for replication
        setParameter: "shardSplitTimeoutMS=100000"
    }
});

test.addAndAwaitRecipientNodes();

const tenantIds = [ObjectId(), ObjectId()];
const operation = test.createSplitOperation(tenantIds);
const donorPrimary = test.donor.getPrimary();
const blockingFP = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");

// Start the shard split and wait until we enter the kBlocking state
const splitThread = operation.commitAsync();
blockingFP.wait();

// Assert there are no blocked writes for tenants so we can confirm there were blocks later
tenantIds.forEach(tenantId =>
                      assert.eq(ShardSplitTest.getNumBlockedWrites(donorPrimary, tenantId), 0));

// Now perform one write for each tenantId being split and wait for the writes to become blocked
const writes = tenantIds.map(tenantId => {
    const writeThread = new Thread(function(primaryConnStr, tenantId) {
        const primary = new Mongo(primaryConnStr);
        const coll = primary.getDB(`${tenantId}_testDb`).testColl;
        const res = coll.insert([{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}],
                                {writeConcern: {w: "majority"}});
        assert.commandFailedWithCode(res, ErrorCodes.TenantMigrationAborted);
    }, donorPrimary.host, tenantId.str);

    writeThread.start();
    return writeThread;
});

// Verify that we have blocked the expected number of writes to tenant data
tenantIds.forEach(tenantId => {
    assert.soon(() => {
        // There are two writes for each insert. The function insertBatchAndHandleErrors first try
        // to acquire the collection lock and create the collection as it doesn't exist. This result
        // in an error that is recorded. However insertBatchAndHandleErrors still try to process the
        // insert, which leads to a second write error.
        const kExpectedBlockedWrites = 2;

        return ShardSplitTest.getNumBlockedWrites(donorPrimary, tenantId) == kExpectedBlockedWrites;
    });
});

// Then abort the operation, disable the failpoint, and assert the operation was aborted
operation.abort();
blockingFP.off();
assert.commandFailedWithCode(splitThread.returnData(), ErrorCodes.TenantMigrationAborted);

// Assert all writes were completed with a TenantMigrationAborted error
writes.forEach(write => write.join());

test.stop();
