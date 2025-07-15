/**
 * Verifies the effect of setting disableResumableRangeDeleter to true on a shard (startup and
 * runtime).
 *
 * requires_persistence - This test restarts shards and expects them to remember their data.
 * @tags: [
 *   requires_persistence,
 *   requires_fcv_82,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test intentionally disables the resumable range deleter.
TestData.skipCheckOrphans = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

Random.setRandomSeed();

let randomStr = () => {
    let N = 2000 + Random.randInt(500);
    let str = '';
    let aCharCode = "a".charCodeAt(0);
    for (let i = 0; i < N; ++i) {
        str = str.concat(String.fromCharCode(aCharCode + Random.randInt(25)));
    }
    return str;
};

function verifyRangeDeleterStatusOnShard(
    st, shardPrimary, toShardName, expectFailure, ns, chunkToMove) {
    // Test 1: Attempt to receive a chunk that would require cleaning an overlapping range.
    // To set this up, we assume a range deletion task was previously created for chunkToMove on
    // this shard.
    const moveChunkRes = st.s.adminCommand(
        {moveChunk: ns, find: chunkToMove, to: toShardName, _waitForDelete: true});
    if (expectFailure) {
        assert.commandFailedWithCode(
            moveChunkRes,
            ErrorCodes.OperationFailed,
            `MoveChunk to ${
                toShardName} should have failed due to ResumableRangeDeleterDisabled. Response: ${
                tojson(moveChunkRes)}`);
    } else {
        assert.commandWorked(
            moveChunkRes,
            `MoveChunk to ${toShardName} should have succeeded. Response: ${tojson(moveChunkRes)}`);
    }

    // Test 2: Attempt cleanupOrphaned on the specific namespace.
    const cleanupRes = shardPrimary.adminCommand({cleanupOrphaned: ns});
    if (expectFailure) {
        assert.commandFailedWithCode(
            cleanupRes,
            ErrorCodes.OrphanedRangeCleanUpFailed,
            `cleanupOrphaned on ${ns} on ${toShardName} should have failed. Response: ${
                tojson(cleanupRes)}`);
    } else {
        assert.commandWorked(
            cleanupRes,
            `cleanupOrphaned on ${ns} on ${toShardName} should have succeeded. Response: ${
                tojson(cleanupRes)}`);
    }

    // Test 3: cleanupOrphaned on an unrelated namespace should always work.
    jsTest.log(`Attempting cleanupOrphaned on unrelated namespace on ${toShardName}`);
    assert.commandWorked(
        shardPrimary.adminCommand({cleanupOrphaned: `${dbName}.unrelated`}),
        `cleanupOrphaned on unrelated namespace on ${toShardName} failed unexpectedly.`);
}

function createRangeDeletionTask(st, shardPrimary, toShardName, chunkToMove) {
    jsTest.log(`Creating range deletion task on ${shardPrimary.shardName} for chunk ${tojson(chunkToMove)}
     by moving to ${toShardName}`);
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: chunkToMove, to: toShardName}));
}

const st = new ShardingTest({shards: 3});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Split into a few chunks across shards
// Chunk1: {_id: MinKey} -> {_id: 0} on shard0
// Chunk2: {_id: 0}      -> {_id: 100} on shard1
// Chunk3: {_id: 100}    -> {_id: MaxKey} on shard2
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 100}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {_id: 100}, to: st.shard2.shardName, _waitForDelete: true}));

const chunkOnShard0 = {
    _id: -10
};  // Belongs to range MinKey -> 0
const chunkOnShard1 = {
    _id: 50
};  // Belongs to range 0 -> 100

jsTest.log("Test Case: Enable/disable the range deleter via the server parameter on a shard");
(() => {
    // Disable the range deleter in shard0 (via runtime) and shard1 (via startup).
    st.shard0.adminCommand({setParameter: 1, disableResumableRangeDeleter: true});
    st.rs1.restart(0, {
        remember: true,
        appendOptions: true,
        startClean: false,
        setParameter: {disableResumableRangeDeleter: true}
    });

    // Create range deletion tasks on shard0 and shard1.
    createRangeDeletionTask(st, st.shard0, st.shard2.shardName, chunkOnShard0);
    createRangeDeletionTask(st, st.shard1, st.shard2.shardName, chunkOnShard1);

    verifyRangeDeleterStatusOnShard(
        st, st.shard0, st.shard0.shardName, true /*expectFailure*/, ns, chunkOnShard0);
    verifyRangeDeleterStatusOnShard(
        st, st.shard1, st.shard1.shardName, true /*expectFailure*/, ns, chunkOnShard1);

    // Enable the range deleter service and verify the status on all shards.
    st.shard0.adminCommand({setParameter: 1, disableResumableRangeDeleter: false});
    st.shard1.adminCommand({setParameter: 1, disableResumableRangeDeleter: false});

    verifyRangeDeleterStatusOnShard(
        st, st.shard0, st.shard0.shardName, false /*expectFailure*/, ns, chunkOnShard0);
    verifyRangeDeleterStatusOnShard(
        st, st.shard1, st.shard1.shardName, false /*expectFailure*/, ns, chunkOnShard1);

    // Clean up: Move chunks back to their original shards for consistency in subsequent tests.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: chunkOnShard0, to: st.shard0.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: chunkOnShard1, to: st.shard1.shardName}));
})();

jsTest.log(
    "Test Case: Disabling the server parameter does not affect moving non-conflicting chunks");
(() => {
    st.shard0.adminCommand({setParameter: 1, disableResumableRangeDeleter: true});

    // Shard0 should be able to donate a chunk (e.g., chunk {_id: -50}) and shard2 should be able to
    // receive it if shard2 has no overlapping tasks.
    const nonConflictingChunk = {_id: -50};
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: nonConflictingChunk, to: st.shard2.shardName}),
        "MoveChunk of non-conflicting range should succeed even if the range deleter is disabled");

    // Clean up: Move it back to shard0 and enable the range deleter.
    st.shard0.adminCommand({setParameter: 1, disableResumableRangeDeleter: false});
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: nonConflictingChunk, to: st.shard0.shardName, _waitForDelete: true}));
})();

jsTest.log(
    "Restart shard0 with a delay between range deletions and a low wait timeout, this way, with a " +
    "large enough collection, there will be a timeout on a recipient when waiting for the range" +
    "deleter task to finish.");
(() => {
    st.rs0.restart(0, {
        remember: true,
        appendOptions: true,
        startClean: false,
        setParameter: {rangeDeleterBatchDelayMS: 60000, rangeDeleterBatchSize: 128}
    });

    st.rs0.getPrimary().adminCommand(
        {setParameter: 1, receiveChunkWaitForRangeDeleterTimeoutMS: 500});

    let bulkOp = st.s.getCollection(ns).initializeUnorderedBulkOp();

    let str = randomStr();
    for (let i = -129; i <= 129; ++i) {
        bulkOp.insert({_id: i, str: str});
    }

    bulkOp.execute();

    // Move a chunk to shard1, this will start the range deletion on shard0.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: chunkOnShard0, to: st.shard1.shardName}));

    // Move the same chunk back, this will make this migration to wait for the range to be deleted.
    jsTest.log(
        "Shard0 should not be able to receive a chunk because the range deletion on an intersecting range is taking too long");
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: chunkOnShard0, to: st.shard0.shardName}),
        ErrorCodes.OperationFailed);
})();

st.stop();
