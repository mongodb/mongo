/**
 * Test that mongos retries SnapshotErrors for non-transaction snapshot reads with no client-
 * provided atClusterTime the same as it does for reads in multi-document transactions, but does
 * *not* retry them if the client provides atClusterTime.
 *
 * In other words:  If the client sends readConcern: {level: "snapshot"} with no atClusterTime,
 * mongos should retry a SnapshotError: it will choose a later timestamp for atClusterTime and may
 * succeed. If the client sends readConcern: {level: "snapshot", atClusterTime: T} and mongos get a
 * SnapshotError, there's no point retrying.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {
    kSnapshotErrors,
    setFailCommandOnShards,
    unsetFailCommandOnEachShard,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const kMinSnapshotHistoryWindowInSeconds = 300;

const kCommandTestCases = [
    {name: "aggregate", command: {aggregate: collName, pipeline: [], cursor: {}}},
    {name: "find", command: {find: collName}},
    {name: "distinct", command: {distinct: collName, query: {}, key: "_id"}},
];

function runTest(st, numShardsToError, errorCode, isSharded) {
    const db = st.s.getDB(dbName);
    const atClusterTime = st.s.adminCommand("hello").operationTime;

    for (let commandTestCase of kCommandTestCases) {
        for (let readConcern of [{level: "snapshot"}, {level: "snapshot", atClusterTime: atClusterTime}]) {
            const commandName = commandTestCase.name;
            const commandBody = commandTestCase.command;
            const failCommandOptions = {namespace: ns, errorCode, failCommands: [commandName]};

            jsTestLog(
                `Test ${commandName},` +
                    ` readConcern ${tojson(readConcern)},` +
                    ` numShardsToError ${numShardsToError},` +
                    ` errorCode ${ErrorCodeStrings[errorCode]},` +
                    ` isSharded ${isSharded}`,
            );

            if (isSharded && commandName === "distinct") {
                // Snapshot distinct isn't allowed on sharded collections.
                print("Skipping distinct test case for sharded collection");
                continue;
            }

            // Clone command so we can modify readConcern.
            let snapshotCommandBody = Object.assign({}, commandBody);
            snapshotCommandBody.readConcern = readConcern;

            if (readConcern.hasOwnProperty("atClusterTime")) {
                // Single error.
                setFailCommandOnShards(st, {times: 1}, failCommandOptions, numShardsToError);
                const res = assert.commandFailedWithCode(db.runCommand(snapshotCommandBody), errorCode);
                // No error labels for non-transaction error.
                assert(!res.hasOwnProperty("errorLabels"));
                unsetFailCommandOnEachShard(st, numShardsToError);
            } else {
                // Single error.
                setFailCommandOnShards(st, {times: 1}, failCommandOptions, numShardsToError);
                assert.commandWorked(db.runCommand(snapshotCommandBody));
                unsetFailCommandOnEachShard(st, numShardsToError);

                // Retry on multiple errors.
                setFailCommandOnShards(st, {times: 3}, failCommandOptions, numShardsToError);
                assert.commandWorked(db.runCommand(snapshotCommandBody));
                unsetFailCommandOnEachShard(st, numShardsToError);

                // Exhaust retry attempts.
                setFailCommandOnShards(st, "alwaysOn", failCommandOptions, numShardsToError);
                const res = assert.commandFailedWithCode(db.runCommand(snapshotCommandBody), errorCode);
                // No error labels for non-transaction error.
                assert(!res.hasOwnProperty("errorLabels"));
                unsetFailCommandOnEachShard(st, numShardsToError);
            }
        }
    }
}

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    config: 1,
    other: {
        rsOptions: {setParameter: {minSnapshotHistoryWindowInSeconds: kMinSnapshotHistoryWindowInSeconds}},
    },
});

jsTestLog("Unsharded snapshot read");

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

for (let errorCode of kSnapshotErrors) {
    runTest(st, 1, errorCode, false /* isSharded */);
}

// Shard collection and set up 2 chunks, [minKey, 10), [10, maxKey), each with
// one document
// (includes the document already inserted).
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 15}, {writeConcern: {w: "majority"}}));

jsTestLog("One shard snapshot read");

assert.eq(2, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard0.shardName}));
assert.eq(0, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard1.shardName}));

for (let errorCode of kSnapshotErrors) {
    runTest(st, 1, errorCode, true /* isSharded */);
}

jsTestLog("Two shard snapshot read");

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 15}, to: st.shard1.shardName}));
assert.eq(1, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard0.shardName}));
assert.eq(1, findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {shard: st.shard1.shardName}));

for (let errorCode of kSnapshotErrors) {
    runTest(st, 2, errorCode, true /* isSharded */);
}

// Test only one shard throwing the error when more than one are targeted.
for (let errorCode of kSnapshotErrors) {
    runTest(st, 1, errorCode, true /* isSharded */);
}

st.stop();
