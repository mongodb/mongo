/**
 * Tests shard targeting for extension stages.
 *
 * Source stage: $readNDocuments desugars to $produceIds (a source stage) followed by
 * $_internalSearchIdLookup. $produceIds provides a filter of {_id: {$gte: 0, $lt: numDocs}} via
 * getFilter(), which the planner uses to determine which shards to target.
 *
 * Transform stage: $testBar is a no-op transform stage that reports its arguments as a filter via
 * getFilter(). The stage passes through all input documents unchanged — the profiler verifies
 * targeting.
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
    {"libread_n_documents_mongo_extension.so": {}, "libbar_mongo_extension.so": {}},
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

        /**
         * Asserts that the given pipeline targets only shard0, not shard1, and returns the
         * expected IDs.
         */
        function assertTargetsShard0Only(pipeline, expectedIds, comment) {
            const results = coll.aggregate(pipeline, {comment}).toArray();
            assert.eq(results.length, expectedIds.length, results);
            assert.sameMembers(
                results.map((d) => d._id),
                expectedIds,
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

        /**
         * Asserts that the given pipeline targets both shards and returns the expected IDs.
         */
        function assertTargetsBothShards(pipeline, expectedIds, comment) {
            const results = coll.aggregate(pipeline, {comment}).toArray();
            assert.eq(results.length, expectedIds.length, results);
            assert.sameMembers(
                results.map((d) => d._id),
                expectedIds,
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

        const idsInRange = (n) => Array.from({length: n}, (_, i) => i);

        // Source stage: filter [0, 3) falls entirely on shard0.
        assertTargetsShard0Only([{$readNDocuments: {numDocs: 3}}], idsInRange(3), "source_single_shard_targeted");

        // Source stage: filter [0, 8) spans both shards.
        assertTargetsBothShards([{$readNDocuments: {numDocs: 8}}], idsInRange(8), "source_both_shards_targeted");

        // Transform stage: filter {_id: {$gte: 0, $lt: 3}} falls entirely on shard0. $testBar is
        // a no-op passthrough, so all docs from shard0 (ids 0-4) are returned.
        assertTargetsShard0Only(
            [{$testBar: {_id: {$gte: 0, $lt: 3}}}],
            idsInRange(5),
            "transform_single_shard_targeted",
        );

        // Transform stage: filter {_id: {$gte: 0, $lt: 8}} spans both shards. All 10 docs
        // returned since $testBar is a no-op passthrough.
        assertTargetsBothShards(
            [{$testBar: {_id: {$gte: 0, $lt: 8}}}],
            idsInRange(10),
            "transform_both_shards_targeted",
        );
    },
    ["sharded"],
    {shards: 2},
);
