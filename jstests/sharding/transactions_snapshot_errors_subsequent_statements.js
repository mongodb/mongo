// Tests mongos behavior on snapshot errors encountered during subsequent statements in a
// multi-statement transaction. In particular, verifies that snapshot errors beyond the first
// command in a transaction are fatal to the entire transaction.
//
// Runs against an unsharded collection, a sharded collection with all chunks on one shard, and a
// sharded collection with one chunk on both shards.
//
// @tags: [requires_sharding, uses_transactions, uses_multi_shard_transaction]
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

const kCommandTestCases = [
    {name: "aggregate", command: {aggregate: collName, pipeline: [], cursor: {}}},
    {name: "distinct", command: {distinct: collName, query: {}, key: "_id"}},
    {name: "find", command: {find: collName}},
    {
        // findAndModify can only target one shard, even in the two shard case.
        name: "findAndModify",
        command: {findAndModify: collName, query: {_id: 1}, update: {$set: {x: 1}}}
    },
    {name: "insert", command: {insert: collName, documents: [{_id: 1}, {_id: 11}]}},
    {
        name: "update",
        command: {
            update: collName,
            updates: [{q: {_id: 1}, u: {$set: {_id: 2}}}, {q: {_id: 11}, u: {$set: {_id: 12}}}]
        }
    },
    {
        name: "delete",
        command: {delete: collName, deletes: [{q: {_id: 2}, limit: 1}, {q: {_id: 12}, limit: 1}]}
    },
    // We cannot test killCursors because mongos discards the response from any killCursors
    // requests that may be sent to shards.
];

function runTest(st, collName, errorCode, isSharded) {
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);

    for (let commandTestCase of kCommandTestCases) {
        const commandName = commandTestCase.name;
        const commandBody = commandTestCase.command;

        if (isSharded && commandName === "distinct") {
            // Distinct isn't allowed on sharded collections in a multi-document transaction.
            print("Skipping distinct test case for sharded collections");
            continue;
        }

        // Successfully start a transaction on one shard.
        session.startTransaction({readConcern: {level: "snapshot"}});
        assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 15}}));

        // Verify the command must fail on a snapshot error from a subsequent statement.
        setFailCommandOnShards(st, {times: 1}, [commandName], errorCode, 1, ns);
        const res = assert.commandFailedWithCode(sessionDB.runCommand(commandBody), errorCode);
        assert.eq(res.errorLabels, ["TransientTransactionError"]);

        assertNoSuchTransactionOnAllShards(
            st, session.getSessionId(), session.getTxnNumber_forTesting());
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    }
}

const st = new ShardingTest({shards: 2, mongos: 1, config: 1});

enableStaleVersionAndSnapshotRetriesWithinTransactions(st);

jsTestLog("Unsharded transaction");

assert.writeOK(st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Single shard case simulates the storage engine discarding an in-use snapshot.
for (let errorCode of kSnapshotErrors) {
    runTest(st, collName, errorCode, false /* isSharded */);
}

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Set up 2 chunks, [minKey, 10), [10, maxKey), each with one document (includes the document
// already inserted).
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
assert.writeOK(st.s.getDB(dbName)[collName].insert({_id: 15}, {writeConcern: {w: "majority"}}));

jsTestLog("One shard transaction");

assert.eq(2, st.s.getDB('config').chunks.count({ns: ns, shard: st.shard0.shardName}));
assert.eq(0, st.s.getDB('config').chunks.count({ns: ns, shard: st.shard1.shardName}));

for (let errorCode of kSnapshotErrors) {
    runTest(st, collName, errorCode, true /* isSharded */);
}

jsTestLog("Two shard transaction");

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 15}, to: st.shard1.shardName}));
assert.eq(1, st.s.getDB('config').chunks.count({ns: ns, shard: st.shard0.shardName}));
assert.eq(1, st.s.getDB('config').chunks.count({ns: ns, shard: st.shard1.shardName}));

// Multi shard case simulates adding a new participant that can no longer support the already
// chosen read timestamp.
for (let errorCode of kSnapshotErrors) {
    runTest(st, collName, errorCode, true /* isSharded */);
}

disableStaleVersionAndSnapshotRetriesWithinTransactions(st);

st.stop();
})();
