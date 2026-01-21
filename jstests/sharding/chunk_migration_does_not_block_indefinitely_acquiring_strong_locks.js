/*
 * Test that chunk migrations don't enqueue strong locks indefinitely when trying to acquire the
 * critical section (they block for up to migrationLockAcquisitionMaxWaitMS).
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {Thread} from "jstests/libs/parallelTester.js";

// Moves the chunk containing the key {x: -1} to shard1 and expect the migration to fail due to
// being unable to take the required strong locks within the migrationLockAcquisitionMaxWaitMS
// timeout. It also checks that writes are not stalled while the migration is attempting to acquire
//  the locks.
function testMigrationFailsAndWritesAreNotBlocked() {
    const fp = configureFailPoint(st.shard0, "hangBeforeEnteringCriticalSection");

    // Start chunk migration from shard0 to shard1
    const moveChunkThread = new Thread(
        function (host, ns, shardName) {
            const conn = new Mongo(host);
            return conn.adminCommand({moveChunk: ns, find: {x: -1}, to: shardName});
        },
        st.s.host,
        ns,
        st.shard1.shardName,
    );

    jsTest.log("Starting migration");
    moveChunkThread.start();

    // Wait for the migration to reach the critical section step on the donor shard.
    jsTest.log("Waiting for migration to reach critical section");
    fp.wait();
    jsTest.log("Migration reached critical section");
    fp.off();

    // Sleep a bit to let the shard enqueue the strong lock acquisitions for the critical section.
    sleep(1000);

    // Check that writes outside of the transaction are not blocked for long, since the migration
    // will remove the enqueued strong locks promptly.
    jsTest.log("Issuing write outside the transaction with maxTimeMS");
    assert.commandWorked(
        db.runCommand({
            update: collName,
            updates: [
                {q: {x: 2}, u: {$set: {y: 1}}},
                {q: {x: -2}, u: {$set: {y: 1}}},
            ],
            maxTimeMS: 10000,
        }),
    );
    jsTest.log("Write completed successfully");

    // Wait for migration to finish.
    jsTest.log("Waiting for migration to complete");
    moveChunkThread.join();
    const moveChunkResult = moveChunkThread.returnData();

    jsTest.log("Migration completed. moveChunk result: " + tojson(moveChunkResult));

    // Expect the migration to fail due to being unable to acquire the locks in time.
    // Note: Donor fails with LockTimeout; recipient fails with CommandFailed.
    assert.commandFailedWithCode(moveChunkResult, [ErrorCodes.LockTimeout, ErrorCodes.CommandFailed]);
}

const st = new ShardingTest({
    shards: 2,
    migrationLockAcquisitionMaxWaitMS: 500,
});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const db = st.s.getDB(dbName);
const coll = db[collName];

// Create sharded collection with two chunks
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));

// Move one chunk to each shard.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -1}, to: st.shard0.shardName, _waitForDelete: true}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName, _waitForDelete: true}));

// Insert test data
assert.commandWorked(coll.insertMany([{x: -2}, {x: -1}, {x: 1}, {x: 2}]));

const session = st.s.startSession();
const sessionColl = session.getDatabase(dbName)[collName];

// Test that the migration recipient correctly applies the migrationLockAcquisitionMaxWaitMS timeout for strong lock acquisition.
{
    // Start a multi-document transaction that targets the recipient shard (shard1).
    session.startTransaction();
    assert.commandWorked(sessionColl.update({x: 1}, {$set: {y: 1}}));

    testMigrationFailsAndWritesAreNotBlocked();

    // Abort the transaction
    assert.commandWorked(session.abortTransaction_forTesting());
}

// Return the chunk to shard0.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -1}, to: st.shard0.shardName, _waitForDelete: true}));

// Test that the migration donor correctly applies the migrationLockAcquisitionMaxWaitMS timeout for strong lock acquisition.
{
    // Start a multi-document transaction that targets the donor shard (shard0).
    session.startTransaction();
    assert.commandWorked(sessionColl.update({x: -1}, {$set: {y: 1}}));

    testMigrationFailsAndWritesAreNotBlocked();

    // Abort the transaction
    assert.commandWorked(session.abortTransaction_forTesting());
}

session.endSession();
st.stop();
