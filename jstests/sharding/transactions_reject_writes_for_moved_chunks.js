// Verify shards reject writes in a transaction to chunks that have moved since the transaction's
// read timestamp.
//
// @tags: [
//   requires_find_command,
//   requires_sharding,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
"use strict";

function expectChunks(st, ns, chunks) {
    for (let i = 0; i < chunks.length; i++) {
        assert.eq(chunks[i],
                  st.s.getDB("config").chunks.count({ns: ns, shard: st["shard" + i].shardName}),
                  "unexpected number of chunks on shard " + i);
    }
}

const dbName = "test";
const collName = "not_hashed";
const ns = dbName + '.' + collName;

const st = new ShardingTest({shards: 3, mongos: 1, config: 1});

// Set up one sharded collection with 2 chunks, both on the primary shard.

assert.writeOK(st.s.getDB(dbName)[collName].insert({_id: -3}, {writeConcern: {w: "majority"}}));
assert.writeOK(st.s.getDB(dbName)[collName].insert({_id: 11}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

expectChunks(st, ns, [2, 0, 0]);

// Force a routing table refresh on Shard2, to avoid picking a global read timestamp before the
// sharding metadata cache collections are created.
assert.commandWorked(st.rs2.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 11}, to: st.shard1.shardName}));

expectChunks(st, ns, [1, 1, 0]);

// The command should target only the second chunk.
let commandTestCases = function(collName) {
    return [
        {
            name: "insert",
            command: {insert: collName, documents: [{_id: 3}]},
        },
        {
            name: "update_query",
            command: {update: collName, updates: [{q: {_id: 11}, u: {$set: {x: 1}}}]},
        },
        {
            name: "update_replacement",
            command: {update: collName, updates: [{q: {_id: 11}, u: {_id: 11, x: 1}}]},
        },
        {
            name: "delete",
            command: {delete: collName, deletes: [{q: {_id: 11}, limit: 1}]},
        },
        {
            name: "findAndModify_update",
            command: {findAndModify: collName, query: {_id: 11}, update: {$set: {x: 1}}},
        },
        {
            name: "findAndModify_delete",
            command: {findAndModify: collName, query: {_id: 11}, remove: true},
        }
    ];
};

function runTest(testCase, collName, moveChunkToFunc, moveChunkBack) {
    const testCaseName = testCase.name;
    const cmdTargetChunk2 = testCase.command;

    jsTestLog("Testing " + testCaseName + ", moveChunkBack: " + moveChunkBack);

    expectChunks(st, ns, [1, 1, 0]);

    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB[collName];

    session.startTransaction({readConcern: {level: "snapshot"}});

    // Start a transaction on Shard0 which will select and pin a global read timestamp.
    assert.eq(sessionColl.find({_id: -3}).itcount(), 1, "expected to find document in first chunk");

    // Move a chunk from Shard1 to Shard2 outside of the transaction. This will happen at a
    // later logical time than the transaction's read timestamp.
    assert.commandWorked(moveChunkToFunc(st.shard2.shardName));

    if (moveChunkBack) {
        // If the chunk is moved back to the shard that owned it at the transaction's read
        // timestamp the later write should still be rejected because conflicting operations may
        // have occurred while the chunk was moved away, which otherwise may not be detected
        // when the shard prepares the transaction.
        assert.commandWorked(moveChunkToFunc(st.shard1.shardName));

        // Flush metadata on the destination shard so the next request doesn't encounter
        // StaleConfig. The router refreshes after moving a chunk, so it will already be fresh.
        assert.commandWorked(
            st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));
    }

    // The write should always fail, but the particular error varies.
    const res = assert.commandFailed(
        sessionDB.runCommand(cmdTargetChunk2),
        "expected write to second chunk to fail, case: " + testCaseName +
            ", cmd: " + tojson(cmdTargetChunk2) + ", moveChunkBack: " + moveChunkBack);

    const errMsg = "write to second chunk failed with unexpected error, res: " + tojson(res) +
        ", case: " + testCaseName + ", cmd: " + tojson(cmdTargetChunk2) +
        ", moveChunkBack: " + moveChunkBack;

    // On slow hosts, this request can always fail with SnapshotTooOld or StaleChunkHistory if
    // a migration takes long enough.
    const expectedCodes = [ErrorCodes.SnapshotTooOld, ErrorCodes.StaleChunkHistory];

    if (testCaseName === "insert") {
        // Insert always inserts a new document, so the only typical error is MigrationConflict.
        expectedCodes.push(ErrorCodes.MigrationConflict);
        assert.commandFailedWithCode(res, expectedCodes, errMsg);
    } else {
        // The other commands modify an existing document so they may also fail with
        // WriteConflict, depending on when orphaned documents are modified.

        if (moveChunkBack) {
            // Orphans from the first migration must have been deleted before the chunk was
            // moved back, so the only typical error is WriteConflict.
            expectedCodes.push(ErrorCodes.WriteConflict);
        } else {
            // If the chunk wasn't moved back, the write races with the range deleter. If the
            // range deleter has not run, the write should fail with MigrationConflict,
            // otherwise with WriteConflict, so both codes are acceptable.
            expectedCodes.push(ErrorCodes.WriteConflict, ErrorCodes.MigrationConflict);
        }
        assert.commandFailedWithCode(res, expectedCodes, errMsg);
    }
    assert.eq(res.errorLabels, ["TransientTransactionError"]);

    // The commit should fail because the earlier write failed.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Move the chunk back to Shard1, if necessary, and reset the database state for the next
    // iteration.
    if (!moveChunkBack) {
        assert.commandWorked(moveChunkToFunc(st.shard1.shardName));
    }

    assert.writeOK(sessionColl.remove({}));
    assert.writeOK(sessionColl.insert([{_id: -3}, {_id: 11}]));
}

let moveNotHashed = function(toShard) {
    return st.s.adminCommand({moveChunk: ns, find: {_id: 11}, to: toShard});
};

commandTestCases(collName).forEach(
    testCase => runTest(testCase, collName, moveNotHashed, false /*moveChunkBack*/));
commandTestCases(collName).forEach(
    testCase => runTest(testCase, collName, moveNotHashed, true /*moveChunkBack*/));

//////////////////////////////////////////////////////////////////////////////////////////////////
//
// Hashed sharding test
// Setup:
// - Command test cases all touch docs that belong to the same chunk that is being moved that
//   originally reside in shard1.
// - The unhashed keys of the docs being touched all map to a chunk in shard1 that never gets
//   moved.

const hashedCollName = "hashed";
const hashedNs = dbName + '.' + hashedCollName;

assert.commandWorked(st.s.adminCommand({shardCollection: hashedNs, key: {_id: 'hashed'}}));

assert.commandWorked(
    st.s.getDB(dbName)[hashedCollName].insert({_id: -3}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[hashedCollName].insert({_id: 11}, {writeConcern: {w: "majority"}}));

let moveHashed = function(toShard) {
    // Use hard coded bounds since the insert and updates in the test case depends on it and
    // we can catch it if the assumption is longer true.
    return st.s.adminCommand({
        moveChunk: hashedNs,
        bounds: [{_id: NumberLong('-3074457345618258602')}, {_id: 0}],
        to: toShard
    });
};

commandTestCases(hashedCollName)
    .forEach(testCase => runTest(testCase, hashedCollName, moveHashed, false /*moveChunkBack*/));
commandTestCases(hashedCollName)
    .forEach(testCase => runTest(testCase, hashedCollName, moveHashed, true /*moveChunkBack*/));

st.stop();
})();
