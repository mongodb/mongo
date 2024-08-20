/**
 * Tests that `onCurrentShardSince` is always consistent with  `history[0].validAfter` on
 * config.chunks entries
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

/* Perform up to 'maxNumSplits' random chunk splits */
function performRandomSplitChunks(st, collFullName, maxNumSplits) {
    for (let i = 0; i < maxNumSplits; i++) {
        // pick a random split point between -50000 and 50000
        const splitPoint = Math.floor(Math.random() * 100000 - 50000);
        if (st.config.chunks.find({"min": {x: NumberLong(splitPoint)}}).count() != 0) {
            continue;
        }
        assert.commandWorked(
            st.s.adminCommand({split: collFullName, middle: {x: NumberLong(splitPoint)}}));
    }
}

/* Create new sharded collection on testDB with up to 201 initial chunks*/
let _collCounter = 0;
function newShardedColl(st, testDB) {
    const coll = testDB["coll_" + _collCounter++];
    assert.commandWorked(st.s.adminCommand({
        shardCollection: coll.getFullName(),
        key: {x: "hashed"},
    }));
    // Perform up to 200 chunk splits
    performRandomSplitChunks(st, coll.getFullName(), 200);
    return coll;
}

/* Perform up to 5 random chunk moves */
function performRandomMoveChunks(st, collFullName) {
    const collUuid = st.s.getDB("config").collections.findOne({_id: collFullName}).uuid;

    function getOppositeShard(shardName) {
        if (shardName === st.shard0.shardName) {
            return st.shard1.shardName;
        } else {
            return st.shard0.shardName;
        }
    }

    const numMoves = Math.floor(Math.random() * 5);
    for (let i = 0; i < numMoves; i++) {
        const chunks = st.config.chunks.find({"uuid": collUuid}).sort({min: 1}).toArray();
        const chunk = chunks[Math.floor(Math.random() * chunks.length)];
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: collFullName, find: chunk.min, to: getOppositeShard(chunk.shard)}));
    }
}

/* Verifies that `onCurrentShardSince` is set and it has the same value as
 * `history[0].validAfter` for each chunk
 */
function assertChunksConsistency(chunksColl) {
    const numTotalChunks = chunksColl.find().count();
    assert.neq(0, numTotalChunks);

    const numConsistenChunks =
        chunksColl
            .find({
                $and: [
                    {"onCurrentShardSince": {$exists: 1}},
                    {
                        $expr: {
                            $eq: [
                                "$onCurrentShardSince",
                                {$getField: {field: "validAfter", input: {$first: "$history"}}}
                            ]
                        }
                    }
                ]
            })
            .count();
    assert.eq(numTotalChunks, numConsistenChunks);
}

function moveAndMergeChunksTest(st, testDB) {
    const coll = newShardedColl(st, testDB);
    const collUuid = st.s.getDB("config").collections.findOne({_id: coll.getFullName()}).uuid;

    // Perform some random moves to have different values on `onCurrentShardSince` fields
    performRandomMoveChunks(st, coll.getFullName());
    assertChunksConsistency(st.config.chunks);

    // Perform at most 10 random merges
    for (let i = 0; i < 10; i++) {
        const chunks = st.config.chunks.find({"uuid": collUuid}).sort({min: 1}).toArray();

        const firstChunkToMergeIndex = Math.floor(Math.random() * (chunks.length - 3));
        const firstChunk = chunks[firstChunkToMergeIndex];
        let lastChunkToMergeIndex = firstChunkToMergeIndex;

        for (let j = 0; j < 3; j++) {
            if (chunks[lastChunkToMergeIndex + 1].shard !== firstChunk.shard) {
                break;
            }
            lastChunkToMergeIndex++;
        }
        if (firstChunkToMergeIndex === lastChunkToMergeIndex) {
            continue;
        }
        const lastChunk = chunks[lastChunkToMergeIndex];

        assert.commandWorked(st.s.adminCommand(
            {mergeChunks: coll.getFullName(), bounds: [firstChunk.min, lastChunk.max]}));
    }

    assertChunksConsistency(st.config.chunks);
}

function splitChunksTest(st, testDB) {
    const coll = newShardedColl(st, testDB);

    // Perform some random moves to have different values on `onCurrentShardSince` fields
    performRandomMoveChunks(st, coll.getFullName());
    assertChunksConsistency(st.config.chunks);

    // Perform at most 10 random splits
    performRandomSplitChunks(st, coll.getFullName(), 10);
    assertChunksConsistency(st.config.chunks);
}

/* Test setup */
const st = new ShardingTest({mongos: 1, shards: 2});
const testDB = st.s.getDB(jsTestName());

/* Perform tests */
if (!TestData.configShard) {
    moveAndMergeChunksTest(st, testDB);
    splitChunksTest(st, testDB);
}

st.stop();
