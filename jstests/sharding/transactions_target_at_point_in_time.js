// Verifies mongos uses a versioned routing table to target subsequent requests in transactions with
// snapshot level read concern.
//
// @tags: [
//   requires_find_command,
//   requires_sharding,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

function expectChunks(st, ns, chunks) {
    for (let i = 0; i < chunks.length; i++) {
        assert.eq(chunks[i],
                  st.s.getDB("config").chunks.count({ns: ns, shard: st["shard" + i].shardName}),
                  "unexpected number of chunks on shard " + i);
    }
}

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({
    shards: 3,
    mongos: 1,
    config: 1,
    other: {
        // Disable expiring old chunk history to ensure the transactions are able to read from a
        // shard that has donated a chunk, even if the migration takes longer than the amount of
        // time for which a chunk's history is normally stored (see SERVER-39763).
        configOptions:
            {setParameter: {"failpoint.skipExpiringOldChunkHistory": "{mode: 'alwaysOn'}"}}
    }
});

// Set up one sharded collection with 2 chunks, both on the primary shard.

assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -5}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

expectChunks(st, ns, [2, 0, 0]);

// Temporarily move a chunk to Shard2, to avoid picking a global read timestamp before the
// sharding metadata cache collections are created.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard2.shardName}));

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
expectChunks(st, ns, [1, 1, 0]);

// First command targets the first chunk, the second command targets the second chunk.
const kCommandTestCases = [
    {
        name: "aggregate",
        commandFuncs: [
            (coll) => coll.aggregate({$match: {_id: -5}}).itcount(),
            (coll) => coll.aggregate({$match: {_id: 5}}).itcount(),
        ]
    },
    {
        name: "find",
        commandFuncs: [
            (coll) => coll.find({_id: -5}).itcount(),
            (coll) => coll.find({_id: 5}).itcount(),
        ]
    }
];

function runTest(testCase) {
    const cmdName = testCase.name;
    const targetChunk1Func = testCase.commandFuncs[0];
    const targetChunk2Func = testCase.commandFuncs[1];

    jsTestLog("Testing " + cmdName);

    expectChunks(st, ns, [1, 1, 0]);

    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB[collName];

    session.startTransaction({readConcern: {level: "snapshot"}});

    // Start a transaction on Shard0 which will select and pin a global read timestamp.
    assert.eq(targetChunk1Func(sessionColl),
              1,
              "expected to find document in first chunk, cmd: " + cmdName);

    // Move a chunk from Shard1 to Shard2 outside of the transaction. This will happen at a
    // later logical time than the transaction's read timestamp.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard2.shardName}));

    // Target a document in the chunk that was moved. The router should get a stale shard
    // version from Shard1 then retry on Shard1 and see the document.
    assert.eq(targetChunk2Func(sessionColl),
              1,
              "expected to find document in second chunk, cmd: " + cmdName);

    assert.commandWorked(session.commitTransaction_forTesting());

    // Move the chunk back to Shard1 for the next iteration.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
}

kCommandTestCases.forEach(runTest);

st.stop();
})();
