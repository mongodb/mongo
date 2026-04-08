/**
 * Tests shard targeting for an extension stage.
 *
 * $readNDocuments desugars to $produceIds (a source stage) followed by $_internalSearchIdLookup.
 * $produceIds provides a filter of {_id: {$gte: 0, $lt: numDocs}} via getFilter(), which the
 * planner uses to determine which shards to target.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsOptimizations,
 * ]
 */
import {profilerHasSingleMatchingEntryOrThrow, profilerHasZeroMatchingEntriesOrThrow} from "jstests/libs/profiler.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

withExtensions(
    {"libread_n_documents_mongo_extension.so": {}},
    (conn, shardingTest) => {
        const dbName = jsTestName();
        const testDB = conn.getDB(dbName);
        const collName = "testColl";
        const coll = testDB[collName];

        // Ensure shard0 is the primary shard.
        assert.commandWorked(
            testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: shardingTest.shard0.shardName}),
        );

        // Shard on _id with a split at 5:
        //   shard0 (primary):     [MinKey, 5) — owns _id 0-4
        //   shard1 (non-primary): [5, MaxKey) — owns _id 5-9
        shardingTest.shardColl(coll, {_id: 1}, {_id: 5}, {_id: 5});

        // Insert 10 documents: _id 0-4 on shard0, _id 5-9 on shard1.
        const docs = Array.from({length: 10}, (_, i) => ({_id: i, val: i * 2}));
        assert.commandWorked(coll.insertMany(docs));

        const shard0DB = shardingTest.shard0.getDB(dbName);
        const shard1DB = shardingTest.shard1.getDB(dbName);

        assert.commandWorked(shard0DB.setProfilingLevel(2));
        assert.commandWorked(shard1DB.setProfilingLevel(2));

        {
            // _id range [0, 3) falls entirely on shard0, so the pipeline should only be sent there.
            const comment = "single_shard_targeted";
            const results = coll.aggregate([{$readNDocuments: {numDocs: 3}}], {comment: comment}).toArray();
            assert.eq(results.length, 3, results);
            assert.sameMembers(
                results.map((d) => d._id),
                [0, 1, 2],
            );

            profilerHasSingleMatchingEntryOrThrow({
                profileDB: shard0DB,
                filter: {
                    "command.aggregate": collName,
                    "command.comment": comment,
                    errCode: {$exists: false},
                },
            });
            profilerHasZeroMatchingEntriesOrThrow({
                profileDB: shard1DB,
                filter: {"command.aggregate": collName, "command.comment": comment},
            });
        }

        {
            // _id range [0, 8) spans both shards, so the pipeline should be sent to both.
            const comment = "both_shards_targeted";
            const results = coll.aggregate([{$readNDocuments: {numDocs: 8}}], {comment: comment}).toArray();
            assert.eq(results.length, 8, results);
            assert.sameMembers(
                results.map((d) => d._id),
                [0, 1, 2, 3, 4, 5, 6, 7],
            );

            profilerHasSingleMatchingEntryOrThrow({
                profileDB: shard0DB,
                filter: {
                    "command.aggregate": collName,
                    "command.comment": comment,
                    errCode: {$exists: false},
                },
            });
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: shard1DB,
                filter: {
                    "command.aggregate": collName,
                    "command.comment": comment,
                    errCode: {$exists: false},
                },
            });
        }
    },
    ["sharded"],
    {shards: 2},
);
