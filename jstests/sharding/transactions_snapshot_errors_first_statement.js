// Tests mongos behavior on snapshot errors encountered during the first statement in a
// multi-statement transaction. In particular, verifies that snapshot errors on the first statement
// in a transaction can be successfully retried on, but that a limit exists on the number of retry
// attempts.
//
// Runs against an unsharded collection, a sharded collection with all chunks on one shard, and a
// sharded collection with one chunk on both shards.
//
// @tags: [requires_sharding, uses_transactions, uses_multi_shard_transaction]
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {
    assertNoSuchTransactionOnAllShards,
    disableStaleVersionAndSnapshotRetriesWithinTransactions,
    enableStaleVersionAndSnapshotRetriesWithinTransactions,
    kSnapshotErrors,
    setFailCommandOnShards,
    unsetFailCommandOnEachShard,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const kCommandTestCases = [
    {name: "aggregate", command: {aggregate: collName, pipeline: [], cursor: {}}},
    {name: "distinct", command: {distinct: collName, query: {}, key: "_id"}},
    {name: "find", command: {find: collName}},
    {
        // findAndModify can only target one shard, even in the two shard case.
        name: "findAndModify",
        command: {findAndModify: collName, query: {_id: 1}, update: {$set: {x: 1}}},
    },
    {name: "insert", command: {insert: collName, documents: [{_id: 1}, {_id: 11}]}},
    {
        name: "update",
        command: {
            update: collName,
            updates: [
                {q: {_id: 1}, u: {$set: {_id: 2}}},
                {q: {_id: 11}, u: {$set: {_id: 12}}},
            ],
        },
    },
    {
        name: "delete",
        command: {
            delete: collName,
            deletes: [
                {q: {_id: 2}, limit: 1},
                {q: {_id: 12}, limit: 1},
            ],
        },
    },
    // We cannot test killCursors because mongos discards the response from any killCursors
    // requests that may be sent to shards.
];

// Verify that all commands that can start a transaction are able to retry on snapshot errors.
function runTest(st, collName, numShardsToError, errorCode, isSharded) {
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);

    for (let commandTestCase of kCommandTestCases) {
        const commandName = commandTestCase.name;
        const commandBody = commandTestCase.command;
        const failCommandOptions = {namespace: ns, errorCode, failCommands: [commandName]};

        if (isSharded && commandName === "distinct") {
            // Distinct isn't allowed on sharded collections in a multi-document transaction.
            print("Skipping distinct test case for sharded collection");
            continue;
        }

        //
        // Retry on a single error.
        //

        setFailCommandOnShards(st, {times: 1}, failCommandOptions, numShardsToError);

        session.startTransaction({readConcern: {level: "snapshot"}});
        assert.commandWorked(sessionDB.runCommand(commandBody));

        assert.commandWorked(session.commitTransaction_forTesting());

        unsetFailCommandOnEachShard(st, numShardsToError);

        // Clean up after insert to avoid duplicate key errors.
        if (commandName === "insert") {
            assert.commandWorked(sessionDB[collName].remove({_id: {$in: [1, 11]}}));
        }

        //
        // Retry on multiple errors.
        //

        setFailCommandOnShards(st, {times: 3}, failCommandOptions, numShardsToError);

        session.startTransaction({readConcern: {level: "snapshot"}});
        assert.commandWorked(sessionDB.runCommand(commandBody));

        assert.commandWorked(session.commitTransaction_forTesting());

        unsetFailCommandOnEachShard(st, numShardsToError);

        // Clean up after insert to avoid duplicate key errors.
        if (commandName === "insert") {
            assert.commandWorked(sessionDB[collName].remove({_id: {$in: [1, 11]}}));
        }

        //
        // Exhaust retry attempts.
        //

        setFailCommandOnShards(st, "alwaysOn", failCommandOptions, numShardsToError);

        session.startTransaction({readConcern: {level: "snapshot"}});
        const res = assert.commandFailedWithCode(sessionDB.runCommand(commandBody), errorCode);
        assert.eq(res.errorLabels, ["TransientTransactionError"]);

        unsetFailCommandOnEachShard(st, numShardsToError);

        assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());

        assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
    }
}

const st = new ShardingTest({shards: 2, mongos: 1});

enableStaleVersionAndSnapshotRetriesWithinTransactions(st);

jsTestLog("Unsharded transaction");

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

for (let errorCode of kSnapshotErrors) {
    runTest(st, collName, 1, errorCode, false /* isSharded */);
}

// Enable sharding and set up 2 chunks, [minKey, 10), [10, maxKey), each with one document
// (includes the document already inserted).
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 15}, {writeConcern: {w: "majority"}}));

jsTestLog("One shard sharded transaction");

assert.eq(2, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard0.shardName}));
assert.eq(0, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard1.shardName}));

for (let errorCode of kSnapshotErrors) {
    runTest(st, collName, 1, errorCode, true /* isSharded */);
}

jsTestLog("Two shard sharded transaction");

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 15}, to: st.shard1.shardName}));
assert.eq(1, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard0.shardName}));
assert.eq(1, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard1.shardName}));

for (let errorCode of kSnapshotErrors) {
    runTest(st, collName, 2, errorCode, true /* isSharded */);
}

// Test only one shard throwing the error when more than one are targeted.
for (let errorCode of kSnapshotErrors) {
    runTest(st, collName, 1, errorCode, true /* isSharded */);
}

disableStaleVersionAndSnapshotRetriesWithinTransactions(st);

st.stop();
