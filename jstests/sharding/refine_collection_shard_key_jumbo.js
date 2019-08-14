//
// Jumbo tests for refineCollectionShardKey.
//

(function() {
'use strict';

const st = new ShardingTest({mongos: 1, shards: 2, other: {chunkSize: 1, enableAutoSplit: true}});
const primaryShard = st.shard0.shardName;
const secondaryShard = st.shard1.shardName;
const kDbName = 'test';
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;
const kConfigChunks = 'config.chunks';
const kZoneName = 'testZone';

function generateJumboChunk() {
    const big = 'X'.repeat(10000);
    let x = 0;
    let bulk = st.s.getCollection(kNsName).initializeUnorderedBulkOp();

    // Create sufficient documents to generate a jumbo chunk, and use the same shard key in all
    // of them so that the chunk cannot be split and moved.
    for (let i = 0; i < 500; i++) {
        bulk.insert({x: x, y: i, big: big});
    }

    assert.commandWorked(bulk.execute());
}

function runBalancer() {
    st.startBalancer();
    let numRounds = 0;

    // Let the balancer run for 3 rounds.
    assert.soon(() => {
        st.awaitBalancerRound();
        st.printShardingStatus(true);
        numRounds++;
        return (numRounds === 3);
    }, 'Balancer failed to run for 3 rounds', 1000 * 60 * 10);

    st.stopBalancer();
}

function validateBalancerBeforeRefine() {
    runBalancer();

    // Confirm that the jumbo chunk has not been split or moved from the primary shard.
    const jumboChunk = st.s.getCollection(kConfigChunks).find({ns: kNsName}).toArray();
    assert.eq(1, jumboChunk.length);
    assert.eq(true, jumboChunk[0].jumbo);
    assert.eq(primaryShard, jumboChunk[0].shard);
}

function validateBalancerAfterRefine() {
    runBalancer();

    // Confirm that the jumbo chunk has been split and some chunks moved to the secondary shard.
    const chunks =
        st.s.getCollection(kConfigChunks)
            .find({ns: kNsName, min: {$lte: {x: 0, y: MaxKey}}, max: {$gt: {x: 0, y: MinKey}}})
            .toArray();
    assert.lt(1, chunks.length);
    assert.eq(true, chunks.some((chunk) => {
        return (chunk.shard === secondaryShard);
    }));
}

function validateMoveChunkBeforeRefine() {
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: kNsName, find: {x: 0}, to: secondaryShard}),
        ErrorCodes.ChunkTooBig);

    // Confirm that the jumbo chunk has not been split or moved from the primary shard.
    const jumboChunk = st.s.getCollection(kConfigChunks).find({ns: kNsName}).toArray();
    assert.eq(1, jumboChunk.length);
    assert.eq(primaryShard, jumboChunk[0].shard);
}

function validateMoveChunkAfterRefine() {
    // Manually split the jumbo chunk before moving the smaller chunks to the secondary shard.
    // We split a total of 4 times to ensure that each chunk is below the max chunk size and so
    // moveChunk succeeds.
    for (let i = 1; i <= 4; i++) {
        assert.commandWorked(st.s.adminCommand({split: kNsName, middle: {x: 0, y: i * 125}}));
    }

    const chunksToMove =
        st.s.getCollection(kConfigChunks)
            .find({ns: kNsName, min: {$lte: {x: 0, y: MaxKey}}, max: {$gt: {x: 0, y: MinKey}}})
            .toArray();
    chunksToMove.forEach((chunk) => {
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: kNsName, find: {x: 0, y: chunk.min.y}, to: secondaryShard}));
    });

    // Confirm that the jumbo chunk has been split and all chunks moved to the secondary shard.
    const chunks =
        st.s.getCollection(kConfigChunks)
            .find({ns: kNsName, min: {$lte: {x: 0, y: MaxKey}}, max: {$gt: {x: 0, y: MinKey}}})
            .toArray();
    assert.lt(1, chunks.length);
    chunks.forEach((chunk) => {
        assert.eq(secondaryShard, chunk.shard);
    });
}

// This test generates a jumbo chunk that cannot be split due to low shard key cardinality. It
// verifies that the balancer cannot split the chunk. After refining the shard key with
// 'refineCollectionShardKey', it verifies that the balancer can now split and move the chunk.
jsTestLog('********** BALANCER JUMBO TEST **********');

// NOTE: The current shard key is {x: 1}.
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, primaryShard);
assert.commandWorked(st.s.adminCommand({shardCollection: kNsName, key: {x: 1}}));

generateJumboChunk();

// Create a zone covering the entire range of shard keys to force the balancer to try and fail
// to move the jumbo chunk.
assert.commandWorked(st.s.adminCommand({addShardToZone: secondaryShard, zone: kZoneName}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: kNsName, min: {x: MinKey}, max: {x: MaxKey}, zone: kZoneName}));

validateBalancerBeforeRefine();

// NOTE: The shard key is now {x: 1, y: 1}.
assert.commandWorked(st.s.getCollection(kNsName).createIndex({x: 1, y: 1}));
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: kNsName, key: {x: 1, y: 1}}));

validateBalancerAfterRefine();

assert.commandWorked(st.s.getDB(kDbName).dropDatabase());

// This test generates a jumbo chunk that cannot be split due to low shard key cardinality. It
// verifies that one cannot manually move the chunk. After refining the shard key with
// 'refineCollectionShardKey', it verifies that one can now manually split and move the chunk.
jsTestLog('********** MANUAL (i.e. MOVE CHUNK) JUMBO TEST **********');

// NOTE: The current shard key is {x: 1}.
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, primaryShard);
assert.commandWorked(st.s.adminCommand({shardCollection: kNsName, key: {x: 1}}));

generateJumboChunk();

validateMoveChunkBeforeRefine();

// NOTE: The shard key is now {x: 1, y: 1}.
assert.commandWorked(st.s.getCollection(kNsName).createIndex({x: 1, y: 1}));
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: kNsName, key: {x: 1, y: 1}}));

validateMoveChunkAfterRefine();

assert.commandWorked(st.s.getDB(kDbName).dropDatabase());

st.stop();
})();
