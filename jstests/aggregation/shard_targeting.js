/**
 * Test that aggregations are sent directly to a single shard in the case where the data required by
 * the pipeline's initial query all resides on that shard, and that we correctly back out and
 * re-target in the event that a stale config exception is received.
 *
 * In particular:
 *
 * - If the data required by the aggregation all resides on a single shard (including multi-chunk
 * range $matches), send the entire pipeline to that shard and do not perform a $mergeCursors.
 * - In the case of a stage which requires a primary shard merge, do not split the pipeline or
 * generate a $mergeCursors if the data required by the aggregation all resides on the primary
 * shard.
 * - In the event that a stale config exception is encountered:
 *     - If the pipeline is split but we now only need to target a single shard, coalesce the split
 *       pipeline and dispatch it to that shard.
 *     - If the pipeline is not split but we must now target more than one shard, split it and
 *       redispatch.
 *
 * Because wrapping these aggregations in a $facet stage will affect how the pipeline is targeted,
 * and will therefore invalidate the results of the test cases below, we tag this test to prevent it
 * running under the 'aggregation_facet_unwind' passthrough.
 *
 * @tags: [do_not_wrap_aggregations_in_facets]
 */
(function() {
    load("jstests/libs/profiler.js");  // For profilerHas*OrThrow helper functions.

    const st = new ShardingTest({shards: 2, mongos: 2, config: 1});

    // mongosForAgg will be used to perform all aggregations.
    // mongosForMove does all chunk migrations, leaving mongosForAgg with stale config metadata.
    const mongosForAgg = st.s0;
    const mongosForMove = st.s1;

    const mongosDB = mongosForAgg.getDB(jsTestName());
    const mongosColl = mongosDB.test;

    const shard0DB = primaryShardDB = st.shard0.getDB(jsTestName());
    const shard1DB = st.shard1.getDB(jsTestName());

    // Turn off best-effort recipient metadata refresh post-migration commit on both shards because
    // it creates non-determinism for the profiler.
    assert.commandWorked(st.shard0.getDB('admin').runCommand(
        {configureFailPoint: 'doNotRefreshRecipientAfterCommit', mode: 'alwaysOn'}));
    assert.commandWorked(st.shard1.getDB('admin').runCommand(
        {configureFailPoint: 'doNotRefreshRecipientAfterCommit', mode: 'alwaysOn'}));

    assert.commandWorked(mongosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), "shard0000");

    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 4 chunks: [MinKey, -100), [-100, 0), [0, 100), [100, MaxKey).
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: -100}}));
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 100}}));

    // Move the [0, 100) and [100, MaxKey) chunks to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 50}, to: "shard0001"}));
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 150}, to: "shard0001"}));

    // Write one document into each of the chunks.
    assert.writeOK(mongosColl.insert({_id: -150}));
    assert.writeOK(mongosColl.insert({_id: -50}));
    assert.writeOK(mongosColl.insert({_id: 50}));
    assert.writeOK(mongosColl.insert({_id: 150}));

    const shardExceptions =
        [ErrorCodes.StaleConfig, ErrorCodes.StaleShardVersion, ErrorCodes.StaleEpoch];

    // Create an $_internalSplitPipeline stage that forces the merge to occur on the Primary shard.
    const forcePrimaryMerge = [{$_internalSplitPipeline: {mergeType: "primaryShard"}}];

    function runAggShardTargetTest({splitPoint}) {
        // Ensure that both mongoS have up-to-date caches, and enable the profiler on both shards.
        assert.commandWorked(mongosForAgg.getDB("admin").runCommand({flushRouterConfig: 1}));
        assert.commandWorked(mongosForMove.getDB("admin").runCommand({flushRouterConfig: 1}));

        assert.commandWorked(shard0DB.setProfilingLevel(2));
        assert.commandWorked(shard1DB.setProfilingLevel(2));

        //
        // Test cases.
        //

        let testName, outColl;

        // Test that a range query is passed through if the chunks encompassed by the query all lie
        // on a single shard, in this case shard0000.
        testName = "agg_shard_targeting_range_single_shard_all_chunks_on_same_shard";
        assert.eq(mongosColl
                      .aggregate([{$match: {_id: {$gte: -150, $lte: -50}}}].concat(splitPoint),
                                 {comment: testName})
                      .itcount(),
                  2);

        // We expect one aggregation on shard0, none on shard1, and no $mergeCursors on shard0 (the
        // primary shard).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard1DB,
            filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: 1}
            }
        });

        // Test that a range query with a stage that requires a primary shard merge ($out in this
        // case) is passed through if the chunk ranges encompassed by the query all lie on the
        // primary shard.
        testName = "agg_shard_targeting_range_all_chunks_on_primary_shard_out_no_merge";
        outColl = mongosDB[testName];

        assert.commandWorked(mongosDB.runCommand({
            aggregate: mongosColl.getName(),
            pipeline: [{$match: {_id: {$gte: -150, $lte: -50}}}].concat(splitPoint).concat([
                {$out: testName}
            ]),
            comment: testName,
            cursor: {}
        }));

        // We expect one aggregation on shard0, none on shard1, and no $mergeCursors on shard0 (the
        // primary shard).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard1DB,
            filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: 1}
            }
        });

        // Verify that the contents of the $out collection are as expected.
        assert.eq(outColl.find().sort({_id: 1}).toArray(), [{_id: -150}, {_id: -50}]);

        // Test that a passthrough will back out and split the pipeline if we try to target a single
        // shard, get a stale config exception, and find that more than one shard is now involved.
        // Move the _id: [-100, 0) chunk from shard0000 to shard0001 via mongosForMove.
        assert.commandWorked(mongosForMove.getDB("admin").runCommand(
            {moveChunk: mongosColl.getFullName(), find: {_id: -50}, to: "shard0001"}));

        // Run the same aggregation that targeted a single shard via the now-stale mongoS. It should
        // attempt to send the aggregation to shard0000, hit a stale config exception, split the
        // pipeline and redispatch. We append an $_internalSplitPipeline stage in order to force a
        // shard merge rather than a mongoS merge.
        testName = "agg_shard_targeting_backout_passthrough_and_split_if_cache_is_stale";
        assert.eq(mongosColl
                      .aggregate([{$match: {_id: {$gte: -150, $lte: -50}}}]
                                     .concat(splitPoint)
                                     .concat(forcePrimaryMerge),
                                 {comment: testName})
                      .itcount(),
                  2);

        // Before the first dispatch:
        // - mongosForMove and shard0000 (the donor shard) are up to date.
        // - mongosForAgg and shard0001 are stale. mongosForAgg incorrectly believes that the
        // necessary data is all on shard0000.
        // We therefore expect that:
        // - mongosForAgg will throw a stale config error when it attempts to establish a
        // single-shard cursor on shard0000 (attempt 1).
        // - mongosForAgg will back out, refresh itself, split the pipeline and redispatch to both
        // shards.
        // - shard0001 will throw a stale config and refresh itself when the split pipeline is sent
        // to it (attempt 2).
        // - mongosForAgg will back out, retain the split pipeline and redispatch (attempt 3).
        // - The aggregation will succeed on the third dispatch.

        // We confirm this behaviour via the following profiler results:

        // - One aggregation on shard0000 with a shard version exception (indicating that the mongoS
        // was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                exceptionCode: {$in: shardExceptions}
            }
        });

        // - One aggregation on shard0001 with a shard version exception (indicating that the shard
        // was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard1DB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                exceptionCode: {$in: shardExceptions}
            }
        });

        // - At most two aggregations on shard0000 with no stale config exceptions. The first, if
        // present, is an aborted cursor created if the command reaches shard0000 before shard0001
        // throws its stale config exception during attempt 2. The second profiler entry is from the
        // aggregation which succeeded.
        profilerHasAtLeastOneAtMostNumMatchingEntriesOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                exceptionCode: {$exists: false}
            },
            maxExpectedMatches: 2
        });

        // - One aggregation on shard0001 with no stale config exception.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard1DB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                exceptionCode: {$exists: false}
            }
        });

        // - One $mergeCursors aggregation on primary shard0000, since we eventually target both
        // shards after backing out the passthrough and splitting the pipeline.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: true}
            }
        });

        // Test that a split pipeline will back out and reassemble the pipeline if we target
        // multiple shards, get a stale config exception, and find that we can now target a single
        // shard.
        // Move the _id: [-100, 0) chunk back from shard0001 to shard0000 via mongosForMove.
        assert.commandWorked(mongosForMove.getDB("admin").runCommand(
            {moveChunk: mongosColl.getFullName(), find: {_id: -50}, to: "shard0000"}));

        // Run the same aggregation via the now-stale mongoS. It should split the pipeline, hit a
        // stale config exception, and reset to the original single-shard pipeline upon refresh. We
        // append an $_internalSplitPipeline stage in order to force a shard merge rather than a
        // mongoS merge.
        testName = "agg_shard_targeting_backout_split_pipeline_and_reassemble_if_cache_is_stale";
        assert.eq(mongosColl
                      .aggregate([{$match: {_id: {$gte: -150, $lte: -50}}}]
                                     .concat(splitPoint)
                                     .concat(forcePrimaryMerge),
                                 {comment: testName})
                      .itcount(),
                  2);

        // Before the first dispatch:
        // - mongosForMove and shard0001 (the donor shard) are up to date.
        // - mongosForAgg and shard0000 are stale. mongosForAgg incorrectly believes that the
        // necessary data is spread across both shards.
        // We therefore expect that:
        // - mongosForAgg will throw a stale config error when it attempts to establish a cursor on
        // shard0001 (attempt 1).
        // - mongosForAgg will back out, refresh itself, coalesce the split pipeline into a single
        // pipeline and redispatch to shard0000.
        // - shard0000 will throw a stale config and refresh itself when the pipeline is sent to it
        // (attempt 2).
        // - mongosForAgg will back out, retain the single-shard pipeline and redispatch (attempt
        // 3).
        // - The aggregation will succeed on the third dispatch.

        // We confirm this behaviour via the following profiler results:

        // - One aggregation on shard0001 with a shard version exception (indicating that the mongoS
        // was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard1DB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                exceptionCode: {$in: shardExceptions}
            }
        });

        // - One aggregation on shard0000 with a shard version exception (indicating that the shard
        // was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                exceptionCode: {$in: shardExceptions}
            }
        });

        // - At most two aggregations on shard0000 with no stale config exceptions. The first, if
        // present, is an aborted cursor created if the command reaches shard0000 before shard0001
        // throws its stale config exception during attempt 1. The second profiler entry is the
        // aggregation which succeeded.
        profilerHasAtLeastOneAtMostNumMatchingEntriesOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                exceptionCode: {$exists: false}
            },
            maxExpectedMatches: 2
        });

        // No $mergeCursors aggregation on primary shard0000, since after backing out the split
        // pipeline we eventually target only shard0000.
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: true}
            }
        });

        // Clean up the test run by dropping the $out collection and resetting the profiler.
        assert(outColl.drop());

        assert.commandWorked(shard0DB.setProfilingLevel(0));
        assert.commandWorked(shard1DB.setProfilingLevel(0));

        assert(shard0DB.system.profile.drop());
        assert(shard1DB.system.profile.drop());
    }

    // Run tests with a variety of splitpoints, testing the pipeline split and re-assembly logic in
    // cases where the merge pipeline is empty, where the split stage is moved from shard to merge
    // pipe ($facet, $lookup), and where there are both shard and merge versions of the split source
    // ($sort, $group, $limit). Each test case will ultimately produce the same output.
    runAggShardTargetTest({splitPoint: []});
    runAggShardTargetTest({splitPoint: [{$sort: {_id: 1}}]});
    runAggShardTargetTest({splitPoint: [{$group: {_id: "$_id"}}]});
    runAggShardTargetTest({splitPoint: [{$limit: 4}]});
    runAggShardTargetTest({
        splitPoint: [
            {$facet: {facetPipe: [{$match: {_id: {$gt: MinKey}}}]}},
            {$unwind: "$facetPipe"},
            {$replaceRoot: {newRoot: "$facetPipe"}}
        ]
    });
    runAggShardTargetTest({
        splitPoint: [
            {
              $lookup: {
                  from: "dummycoll",
                  localField: "dummyfield",
                  foreignField: "dummyfield",
                  as: "lookupRes"
              }
            },
            {$project: {lookupRes: 0}}
        ]
    });

    st.stop();
})();
