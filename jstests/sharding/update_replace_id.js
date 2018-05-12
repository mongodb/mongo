/**
 * Test to confirm that mongoS's special handling of replacement updates with an exact query on _id
 * behaves as expected in the case where a collection's shard key includes _id:
 *
 * - For update replacements, mongoS combines the _id from the query with the replacement document
 * to target the query towards a single shard, rather than scattering to all shards.
 * - For upsert replacements, which always require an exact shard key match, mongoS combines the _id
 * from the query with the replacement document to produce a complete shard key.
 *
 * These special cases are allowed because mongoD always propagates the _id of an existing document
 * into its replacement, and in the case of an upsert will use the value of _id from the query
 * filter.
 */
(function() {
    load("jstests/libs/profiler.js");  // For profilerHas*OrThrow helper functions.

    const st = new ShardingTest({shards: 2, mongos: 1, config: 1, other: {enableBalancer: false}});

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB.test;

    const shard0DB = st.shard0.getDB(jsTestName());
    const shard1DB = st.shard1.getDB(jsTestName());

    assert.commandWorked(mongosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

    // Enables profiling on both shards so that we can verify the targeting behaviour.
    function restartProfiling() {
        for (let shardDB of[shard0DB, shard1DB]) {
            shardDB.setProfilingLevel(0);
            shardDB.system.profile.drop();
            shardDB.setProfilingLevel(2);
        }
    }

    // Run the set of tests relevant to the given shardKey.
    function runReplacementUpdateTests(shardKey) {
        // Write a single document to shard0 and verify that it is present.
        mongosColl.insert({_id: -100, a: -100, msg: "not_updated"});
        assert.docEq(shard0DB.test.find({_id: -100}).toArray(),
                     [{_id: -100, a: -100, msg: "not_updated"}]);

        // Write a document with the same key directly to shard1. This simulates an orphaned
        // document, or the duplicate document which temporarily exists during a chunk migration.
        shard1DB.test.insert({_id: -100, a: -100, msg: "not_updated"});

        // Clear and restart the profiler on both shards.
        restartProfiling();

        // Perform a replacement update whose query is an exact match on _id and whose replacement
        // document contains the remainder of the shard key. Despite the fact that the replacement
        // document does not contain the entire shard key, we expect that mongoS will extract the
        // _id from the query and combine it with the replacement doc to target a single shard.
        let writeRes = assert.commandWorked(
            mongosColl.update({_id: -100}, {a: -100, msg: "update_extracted_id_from_query"}));

        // Verify that the update did not modify the orphan document.
        assert.docEq(shard1DB.test.find({_id: -100}).toArray(),
                     [{_id: -100, a: -100, msg: "not_updated"}]);
        assert.eq(writeRes.nMatched, 1);
        assert.eq(writeRes.nModified, 1);

        // Verify that the update only targeted shard0 and that the resulting document appears as
        // expected.
        assert.docEq(mongosColl.find({_id: -100}).toArray(),
                     [{_id: -100, a: -100, msg: "update_extracted_id_from_query"}]);
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {op: "update", "command.u.msg": "update_extracted_id_from_query"}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard1DB,
            filter: {op: "update", "command.u.msg": "update_extracted_id_from_query"}
        });

        // Perform an upsert replacement whose query is an exact match on _id and whose replacement
        // doc contains the remainder of the shard key. The _id taken from the query should be used
        // both in targeting the update and in generating the new document.
        writeRes = assert.commandWorked(mongosColl.update(
            {_id: 101}, {a: 101, msg: "upsert_extracted_id_from_query"}, {upsert: true}));
        assert.eq(writeRes.nUpserted, 1);

        // Verify that the update only targeted shard1, and that the resulting document appears as
        // expected. At this point in the test we expect shard1 to be stale, because it was the
        // destination shard for the first moveChunk; we therefore explicitly check the profiler for
        // a successful update, i.e. one which did not report a stale config exception.
        assert.docEq(mongosColl.find({_id: 101}).toArray(),
                     [{_id: 101, a: 101, msg: "upsert_extracted_id_from_query"}]);
        assert.docEq(shard1DB.test.find({_id: 101}).toArray(),
                     [{_id: 101, a: 101, msg: "upsert_extracted_id_from_query"}]);
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard0DB,
            filter: {op: "update", "command.u.msg": "upsert_extracted_id_from_query"}
        });
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard1DB,
            filter: {
                op: "update",
                "command.u.msg": "upsert_extracted_id_from_query",
                errName: {$exists: false}
            }
        });

        // The remainder of the tests are only relevant for a compound shard key. If the key is
        // {_id: 'hashed'}, we stop at this point.
        if (shardKey._id === "hashed") {
            return;
        }

        // Verify that an update whose query contains an exact match on _id but whose replacement
        // doc does not contain all other shard key fields will be rejected by mongoS.
        writeRes = assert.commandFailedWithCode(
            mongosColl.update({_id: -100, a: -100}, {msg: "update_failed_missing_shard_key_field"}),
            ErrorCodes.ShardKeyNotFound);

        // Check that the existing document remains unchanged, and that the update did not reach
        // either shard per their respective profilers.
        assert.docEq(mongosColl.find({_id: -100, a: -100}).toArray(),
                     [{_id: -100, a: -100, msg: "update_extracted_id_from_query"}]);
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard0DB,
            filter: {op: "update", "command.u.msg": "update_failed_missing_shard_key_field"}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard1DB,
            filter: {op: "update", "command.u.msg": "update_failed_missing_shard_key_field"}
        });

        // Verify that an upsert whose query contains an exact match on _id but whose replacement
        // document does not contain all other shard key fields will be rejected by mongoS, since it
        // does not contain an exact shard key match.
        writeRes = assert.commandFailedWithCode(
            mongosColl.update({_id: 200, a: 200}, {msg: "upsert_targeting_failed"}, {upsert: true}),
            ErrorCodes.ShardKeyNotFound);
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard0DB,
            filter: {op: "update", "command.u.msg": "upsert_targeting_failed"}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard1DB,
            filter: {op: "update", "command.u.msg": "upsert_targeting_failed"}
        });
        assert.eq(mongosColl.find({_id: 200, a: 200}).itcount(), 0);
    }

    // Shard the test collection on {_id: 1, a: 1}, split it into two chunks, and migrate one of
    // these to the second shard.
    st.shardColl(
        mongosColl, {_id: 1, a: 1}, {_id: 0, a: 0}, {_id: 1, a: 1}, mongosDB.getName(), true);

    // Run the replacement behaviour tests that are relevant to a compound key that includes _id.
    runReplacementUpdateTests({_id: 1, a: 1});

    // Drop and reshard the collection on {_id: "hashed"}, which will autosplit across both shards.
    assert(mongosColl.drop());
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: "hashed"}});

    // Run the replacement behaviour tests relevant to a collection sharded on {_id: "hashed"}.
    runReplacementUpdateTests({_id: "hashed"});

    st.stop();
})();