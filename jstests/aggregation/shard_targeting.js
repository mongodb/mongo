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
 *
 * Because wrapping these aggregations in a $facet stage will affect how the pipeline is targeted,
 * and will therefore invalidate the results of the test cases below, we tag this test to prevent it
 * running under the 'aggregation_facet_unwind' passthrough.
 *
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_sharding,
 *   requires_spawning_own_processes,
 * ]
 */
(function() {
    load("jstests/libs/profiler.js");  // For profilerHas*OrThrow helper functions.

    const st = new ShardingTest({shards: 2, merizos: 2, config: 1});

    // merizosForAgg will be used to perform all aggregations.
    // merizosForMove does all chunk migrations, leaving merizosForAgg with stale config metadata.
    const merizosForAgg = st.s0;
    const merizosForMove = st.s1;

    const merizosDB = merizosForAgg.getDB(jsTestName());
    const merizosColl = merizosDB.test;

    const shard0DB = primaryShardDB = st.shard0.getDB(jsTestName());
    const shard1DB = st.shard1.getDB(jsTestName());

    // Turn off best-effort recipient metadata refresh post-migration commit on both shards because
    // it creates non-determinism for the profiler.
    assert.commandWorked(st.shard0.getDB('admin').runCommand(
        {configureFailPoint: 'doNotRefreshRecipientAfterCommit', mode: 'alwaysOn'}));
    assert.commandWorked(st.shard1.getDB('admin').runCommand(
        {configureFailPoint: 'doNotRefreshRecipientAfterCommit', mode: 'alwaysOn'}));

    // Turn off automatic shard refresh in merizos when a stale config error is thrown.
    assert.commandWorked(merizosForAgg.getDB('admin').runCommand(
        {configureFailPoint: 'doNotRefreshShardsOnRetargettingError', mode: 'alwaysOn'}));

    assert.commandWorked(merizosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    st.ensurePrimaryShard(merizosDB.getName(), st.shard0.shardName);

    // Shard the test collection on _id.
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 4 chunks: [MinKey, -100), [-100, 0), [0, 100), [100, MaxKey).
    assert.commandWorked(
        merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {_id: -100}}));
    assert.commandWorked(
        merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(
        merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {_id: 100}}));

    // Move the [0, 100) and [100, MaxKey) chunks to st.shard1.shardName.
    assert.commandWorked(merizosDB.adminCommand(
        {moveChunk: merizosColl.getFullName(), find: {_id: 50}, to: st.shard1.shardName}));
    assert.commandWorked(merizosDB.adminCommand(
        {moveChunk: merizosColl.getFullName(), find: {_id: 150}, to: st.shard1.shardName}));

    // Write one document into each of the chunks.
    assert.writeOK(merizosColl.insert({_id: -150}));
    assert.writeOK(merizosColl.insert({_id: -50}));
    assert.writeOK(merizosColl.insert({_id: 50}));
    assert.writeOK(merizosColl.insert({_id: 150}));

    const shardExceptions =
        [ErrorCodes.StaleConfig, ErrorCodes.StaleShardVersion, ErrorCodes.StaleEpoch];

    // Create an $_internalSplitPipeline stage that forces the merge to occur on the Primary shard.
    const forcePrimaryMerge = [{$_internalSplitPipeline: {mergeType: "primaryShard"}}];

    function runAggShardTargetTest({splitPoint}) {
        // Ensure that both merizoS have up-to-date caches, and enable the profiler on both shards.
        assert.commandWorked(merizosForAgg.getDB("admin").runCommand({flushRouterConfig: 1}));
        assert.commandWorked(merizosForMove.getDB("admin").runCommand({flushRouterConfig: 1}));

        assert.commandWorked(shard0DB.setProfilingLevel(2));
        assert.commandWorked(shard1DB.setProfilingLevel(2));

        //
        // Test cases.
        //

        let testName, outColl;

        // Test that a range query is passed through if the chunks encompassed by the query all lie
        // on a single shard, in this case st.shard0.shardName.
        testName = "agg_shard_targeting_range_single_shard_all_chunks_on_same_shard";
        assert.eq(merizosColl
                      .aggregate([{$match: {_id: {$gte: -150, $lte: -50}}}].concat(splitPoint),
                                 {comment: testName})
                      .itcount(),
                  2);

        // We expect one aggregation on shard0, none on shard1, and no $mergeCursors on shard0 (the
        // primary shard).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {"command.aggregate": merizosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard1DB,
            filter: {"command.aggregate": merizosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: 1}
            }
        });

        // Test that a range query with a stage that requires a primary shard merge ($out in this
        // case) is passed through if the chunk ranges encompassed by the query all lie on the
        // primary shard.
        testName = "agg_shard_targeting_range_all_chunks_on_primary_shard_out_no_merge";
        outColl = merizosDB[testName];

        assert.commandWorked(merizosDB.runCommand({
            aggregate: merizosColl.getName(),
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
            filter: {"command.aggregate": merizosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: shard1DB,
            filter: {"command.aggregate": merizosColl.getName(), "command.comment": testName}
        });
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: 1}
            }
        });

        // Verify that the contents of the $out collection are as expected.
        assert.eq(outColl.find().sort({_id: 1}).toArray(), [{_id: -150}, {_id: -50}]);

        // Test that a passthrough will back out and split the pipeline if we try to target a single
        // shard, get a stale config exception, and find that more than one shard is now involved.
        // Move the _id: [-100, 0) chunk from st.shard0.shardName to st.shard1.shardName via
        // merizosForMove.
        assert.commandWorked(merizosForMove.getDB("admin").runCommand({
            moveChunk: merizosColl.getFullName(),
            find: {_id: -50},
            to: st.shard1.shardName,
        }));

        // Run the same aggregation that targeted a single shard via the now-stale merizoS. It should
        // attempt to send the aggregation to st.shard0.shardName, hit a stale config exception,
        // split the pipeline and redispatch. We append an $_internalSplitPipeline stage in order to
        // force a shard merge rather than a merizoS merge.
        testName = "agg_shard_targeting_backout_passthrough_and_split_if_cache_is_stale";
        assert.eq(merizosColl
                      .aggregate([{$match: {_id: {$gte: -150, $lte: -50}}}]
                                     .concat(splitPoint)
                                     .concat(forcePrimaryMerge),
                                 {comment: testName})
                      .itcount(),
                  2);

        // Before the first dispatch:
        // - merizosForMove and st.shard0.shardName (the donor shard) are up to date.
        // - merizosForAgg and st.shard1.shardName are stale. merizosForAgg incorrectly believes that
        //   the necessary data is all on st.shard0.shardName.
        //
        // We therefore expect that:
        // - merizosForAgg will throw a stale config error when it attempts to establish a
        // single-shard cursor on st.shard0.shardName (attempt 1).
        // - merizosForAgg will back out, refresh itself, and redispatch to both shards.
        // - st.shard1.shardName will throw a stale config and refresh itself when the split
        // pipeline is sent to it (attempt 2).
        // - merizosForAgg will back out and redispatch (attempt 3).
        // - The aggregation will succeed on the third dispatch.

        // We confirm this behaviour via the following profiler results:

        // - One aggregation on st.shard0.shardName with a shard version exception (indicating that
        // the merizoS was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                errCode: {$in: shardExceptions}
            }
        });

        // - One aggregation on st.shard1.shardName with a shard version exception (indicating that
        // the shard was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard1DB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                errCode: {$in: shardExceptions}
            }
        });

        // - At most two aggregations on st.shard0.shardName with no stale config exceptions. The
        // first, if present, is an aborted cursor created if the command reaches
        // st.shard0.shardName before st.shard1.shardName throws its stale config exception during
        // attempt 2. The second profiler entry is from the aggregation which succeeded.
        profilerHasAtLeastOneAtMostNumMatchingEntriesOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                errCode: {$exists: false}
            },
            maxExpectedMatches: 2
        });

        // - One aggregation on st.shard1.shardName with no stale config exception.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard1DB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                errCode: {$exists: false}
            }
        });

        // - One $mergeCursors aggregation on primary st.shard0.shardName, since we eventually
        // target both shards after backing out the passthrough and splitting the pipeline.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: true}
            }
        });

        // Move the _id: [-100, 0) chunk back from st.shard1.shardName to st.shard0.shardName via
        // merizosForMove. Shard0 and merizosForAgg are now stale.
        assert.commandWorked(merizosForMove.getDB("admin").runCommand({
            moveChunk: merizosColl.getFullName(),
            find: {_id: -50},
            to: st.shard0.shardName,
            _waitForDelete: true
        }));

        // Run the same aggregation via the now-stale merizoS. It should split the pipeline, hit a
        // stale config exception, and reset to the original single-shard pipeline upon refresh. We
        // append an $_internalSplitPipeline stage in order to force a shard merge rather than a
        // merizoS merge.
        testName = "agg_shard_targeting_backout_split_pipeline_and_reassemble_if_cache_is_stale";
        assert.eq(merizosColl
                      .aggregate([{$match: {_id: {$gte: -150, $lte: -50}}}]
                                     .concat(splitPoint)
                                     .concat(forcePrimaryMerge),
                                 {comment: testName})
                      .itcount(),
                  2);

        // Before the first dispatch:
        // - merizosForMove and st.shard1.shardName (the donor shard) are up to date.
        // - merizosForAgg and st.shard0.shardName are stale. merizosForAgg incorrectly believes that
        // the necessary data is spread across both shards.
        //
        // We therefore expect that:
        // - merizosForAgg will throw a stale config error when it attempts to establish a cursor on
        // st.shard1.shardName (attempt 1).
        // - merizosForAgg will back out, refresh itself, and redispatch to st.shard0.shardName.
        // - st.shard0.shardName will throw a stale config and refresh itself when the pipeline is
        // sent to it (attempt 2).
        // - merizosForAgg will back out, and redispatch (attempt 3).
        // - The aggregation will succeed on the third dispatch.

        // We confirm this behaviour via the following profiler results:

        // - One aggregation on st.shard1.shardName with a shard version exception (indicating that
        // the merizoS was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard1DB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                errCode: {$in: shardExceptions}
            }
        });

        // - One aggregation on st.shard0.shardName with a shard version exception (indicating that
        // the shard was stale).
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                errCode: {$in: shardExceptions}
            }
        });

        // - At most two aggregations on st.shard0.shardName with no stale config exceptions. The
        // first, if present, is an aborted cursor created if the command reaches
        // st.shard0.shardName before st.shard1.shardName throws its stale config exception during
        // attempt 1. The second profiler entry is the aggregation which succeeded.
        profilerHasAtLeastOneAtMostNumMatchingEntriesOrThrow({
            profileDB: shard0DB,
            filter: {
                "command.aggregate": merizosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: false},
                errCode: {$exists: false}
            },
            maxExpectedMatches: 2
        });

        // No $mergeCursors aggregation on primary st.shard0.shardName, since after backing out the
        // split pipeline we eventually target only st.shard0.shardName.
        profilerHasZeroMatchingEntriesOrThrow({
            profileDB: primaryShardDB,
            filter: {
                "command.aggregate": merizosColl.getName(),
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
