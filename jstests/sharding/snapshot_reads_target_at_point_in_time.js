// Verifies mongos uses a versioned routing table to target subsequent requests for snapshot reads.
//
// @tags: [
//   requires_fcv_47,
//   requires_find_command,
//   requires_persistence,
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
        rs0: {nodes: 2},
        rs1: {nodes: 2},
        rs2: {nodes: 2},
        // Disable expiring old chunk history to ensure the transactions are able to read from a
        // shard that has donated a chunk, even if the migration takes longer than the amount of
        // time for which a chunk's history is normally stored (see SERVER-39763).
        configOptions: {
            setParameter: {
                "failpoint.skipExpiringOldChunkHistory": "{mode: 'alwaysOn'}",
                minSnapshotHistoryWindowInSeconds: 600
            }
        },
        rsOptions: {setParameter: {minSnapshotHistoryWindowInSeconds: 600}}
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
        commands: [
            {aggregate: collName, pipeline: [{$match: {_id: -5}}], cursor: {}},
            {aggregate: collName, pipeline: [{$match: {_id: 5}}], cursor: {}}
        ]
    },
    {
        name: "find",
        commands: [{find: collName, filter: {_id: -5}}, {find: collName, filter: {_id: 5}}]
    }
];

const TestMode = {
    TRANSACTION: 'TRANSACTION',
    CAUSAL_CONSISTENCY: 'CAUSAL_CONSISTENCY',
    SNAPSHOT: 'SNAPSHOT',
    SNAPSHOT_AT_CLUSTER_TIME: 'SNAPSHOT_AT_CLUSTER_TIME'
};

function runTest(testCase, testMode, readPreferenceMode) {
    const cmdName = testCase.name;
    // Clone commands so we can modify readConcern and readPreference.
    const targetChunk1Cmd = Object.assign({}, testCase.commands[0]);
    const targetChunk2Cmd = Object.assign({}, testCase.commands[1]);
    targetChunk1Cmd["$readPreference"] = {mode: readPreferenceMode};
    targetChunk2Cmd["$readPreference"] = {mode: readPreferenceMode};

    jsTestLog(`Testing ${cmdName} in mode ${testMode}`);

    expectChunks(st, ns, [1, 1, 0]);

    st.refreshCatalogCacheForNs(st.s, ns);

    let session, db;
    switch (testMode) {
        case TestMode.TRANSACTION:
            session = st.s.startSession({causalConsistency: false});
            session.startTransaction({readConcern: {level: "snapshot"}});
            db = session.getDatabase(dbName);
            break;
        case TestMode.CAUSAL_CONSISTENCY:
            session = st.s.startSession({causalConsistency: true});
            db = session.getDatabase(dbName);
            db[collName].findOne();  // Establish a timestamp in the session.
            break;
        case TestMode.SNAPSHOT:
            db = st.s.getDB(dbName);
            targetChunk1Cmd.readConcern = targetChunk2Cmd.readConcern = {level: "snapshot"};
            break;
        case TestMode.SNAPSHOT_AT_CLUSTER_TIME:
            db = st.s.getDB(dbName);
            const opTime = st.s.getDB(dbName).runCommand({ping: 1}).operationTime;
            targetChunk1Cmd.readConcern = {level: "snapshot", atClusterTime: opTime};
            break;
    }

    // Establish a read timestamp.
    let res = assert.commandWorked(db.runCommand(targetChunk1Cmd));
    assert.sameMembers([{_id: -5}],
                       res.cursor.firstBatch,
                       `expected to find document in first chunk, command` +
                           ` ${tojson(targetChunk1Cmd)} returned ${tojson(res)}`);

    const targetChunk1CmdTimestamp = res.cursor.atClusterTime;
    jsTestLog(`Chunk 1 command replied with timestamp ${tojson(targetChunk1CmdTimestamp)}`);

    // Move a chunk from Shard1 to Shard2 outside of the transaction, and update it. This will
    // happen at a later logical time than the read timestamp.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard2.shardName}));

    res = assert.commandWorked(st.s.getDB(dbName).runCommand({
        update: collName,
        updates: [{q: {_id: 5}, u: {$set: {x: true}}}],
        writeConcern: {w: "majority"}
    }));
    jsTestLog(`Updated chunk 2 at timestamp ${tojson(res.operationTime)}`);
    st.refreshCatalogCacheForNs(st.s, ns);

    if (testMode === TestMode.SNAPSHOT_AT_CLUSTER_TIME) {
        targetChunk2Cmd.readConcern = {level: "snapshot", atClusterTime: targetChunk1CmdTimestamp};
    }

    res = assert.commandWorked(db.runCommand(targetChunk2Cmd));

    switch (testMode) {
        case TestMode.CAUSAL_CONSISTENCY:
        case TestMode.SNAPSHOT:
            // We may or may not see the result of the update above.
            assert.eq(1,
                      res.cursor.firstBatch.length,
                      `expected to find document in second chunk, command` +
                          ` ${tojson(targetChunk2Cmd)} returned ${tojson(res)}`);
            assert.eq(5,
                      res.cursor.firstBatch[0]._id,
                      `expected to find {_id: 5} in second chunk, command` +
                          ` ${tojson(targetChunk2Cmd)} returned ${tojson(res)}`);
            break;
        case TestMode.TRANSACTION:
        case TestMode.SNAPSHOT_AT_CLUSTER_TIME:
            // Must not see the update's result.
            assert.sameMembers([{_id: 5}],
                               res.cursor.firstBatch,
                               `expected to find document in second chunk, command` +
                                   ` ${tojson(targetChunk2Cmd)} returned ${tojson(res)}`);
            break;
    }

    if (testMode === TestMode.TRANSACTION) {
        assert.commandWorked(session.commitTransaction_forTesting());
    }

    // Move the chunk back to Shard1 and clear updated field for the next iteration.
    assert.commandWorked(st.s.getDB(dbName).runCommand({
        update: collName,
        updates: [{q: {_id: 5}, u: {$unset: {x: true}}}],
        writeConcern: {w: "majority"}
    }));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
}

for (let testCase of kCommandTestCases) {
    for (let testMode of Object.values(TestMode)) {
        for (let readPreferenceMode of ["primary", "secondary"]) {
            if (readPreferenceMode === "secondary" && testMode === TestMode.TRANSACTION) {
                // Transactions aren't supported on secondaries.
                continue;
            }

            runTest(testCase, testMode, readPreferenceMode);
        }
    }
}
st.stop();
})();
