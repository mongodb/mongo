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
    const collName = "foo";
    const ns = dbName + '.' + collName;

    const st = new ShardingTest({shards: 3, mongos: 1, config: 1});

    // Set up one sharded collection with 2 chunks, both on the primary shard.

    assert.writeOK(st.s.getDB(dbName)[collName].insert({_id: -5}, {writeConcern: {w: "majority"}}));
    assert.writeOK(st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

    expectChunks(st, ns, [2, 0, 0]);

    // Force a routing table refresh on Shard2, to avoid picking a global read timestamp before the
    // sharding metadata cache collections are created.
    assert.commandWorked(st.rs2.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));

    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));

    expectChunks(st, ns, [1, 1, 0]);

    // The command should target only the second chunk.
    const kCommandTestCases = [
        {
          name: "insert",
          command: {insert: collName, documents: [{_id: 6}]},
        },
        {
          name: "update_query",
          command: {update: collName, updates: [{q: {_id: 5}, u: {$set: {x: 1}}}]},
        },
        {
          name: "update_replacement",
          command: {update: collName, updates: [{q: {_id: 5}, u: {_id: 5, x: 1}}]},
        },
        {
          name: "delete",
          command: {delete: collName, deletes: [{q: {_id: 5}, limit: 1}]},
        },
        {
          name: "findAndModify_update",
          command: {findAndModify: collName, query: {_id: 5}, update: {$set: {x: 1}}},
        },
        {
          name: "findAndModify_delete",
          command: {findAndModify: collName, query: {_id: 5}, remove: true},
        }
    ];

    function runTest(testCase, moveChunkBack) {
        const testCaseName = testCase.name;
        const cmdTargetChunk2 = testCase.command;

        jsTestLog("Testing " + testCaseName + ", moveChunkBack: " + moveChunkBack);

        expectChunks(st, ns, [1, 1, 0]);

        const session = st.s.startSession();
        const sessionDB = session.getDatabase(dbName);
        const sessionColl = sessionDB[collName];

        session.startTransaction({readConcern: {level: "snapshot"}});

        // Start a transaction on Shard0 which will select and pin a global read timestamp.
        assert.eq(
            sessionColl.find({_id: -5}).itcount(), 1, "expected to find document in first chunk");

        // Move a chunk from Shard1 to Shard2 outside of the transaction. This will happen at a
        // later logical time than the transaction's read timestamp.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard2.shardName}));

        if (moveChunkBack) {
            // If the chunk is moved back to the shard that owned it at the transaction's read
            // timestamp the later write should still be rejected because conflicting operations may
            // have occurred while the chunk was moved away, which otherwise may not be detected
            // when the shard prepares the transaction.
            assert.commandWorked(
                st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
        }

        // Should fail with NoSuchTransaction, because the write fails with a snapshot error, which
        // won't be retryable because we're on a subsequent statement.
        assert.commandFailedWithCode(sessionDB.runCommand(cmdTargetChunk2),
                                     ErrorCodes.NoSuchTransaction,
                                     "expected write to second chunk to fail, case: " +
                                         testCaseName + ", cmd: " + tojson(cmdTargetChunk2) +
                                         ", moveChunkBack: " + moveChunkBack);

        // The commit should fail because the earlier write failed.
        assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);

        // Move the chunk back to Shard1, if necessary, and reset the database state for the next
        // iteration.
        if (!moveChunkBack) {
            assert.commandWorked(
                st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
        }
        assert.writeOK(sessionColl.remove({}));
        assert.writeOK(sessionColl.insert([{_id: 5}, {_id: -5}]));
    }

    kCommandTestCases.forEach(testCase => runTest(testCase, false /*moveChunkBack*/));
    kCommandTestCases.forEach(testCase => runTest(testCase, true /*moveChunkBack*/));

    st.stop();
})();
