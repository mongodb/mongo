/**
 * Tests that split aggregations whose merge pipelines are eligible to run on mongoS do so, and
 * produce the expected results.
 *
 * Splittable stages whose merge components are eligible to run on mongoS include:
 * - $sort (iff merging pre-sorted streams)
 * - $skip
 * - $limit
 * - $sample
 *
 * Non-splittable stages such as those listed below are eligible to run in a mongoS merge pipeline:
 * - $match
 * - $project
 * - $addFields
 * - $unwind
 * - $redact
 *
 * Because wrapping these aggregations in a $facet stage will affect how the pipeline can be merged,
 * and will therefore invalidate the results of the test cases below, we tag this test to prevent it
 * running under the 'aggregation_facet_unwind' passthrough.
 *
 * @tags: [do_not_wrap_aggregations_in_facets]
 */

(function() {
    load("jstests/libs/profiler.js");  // For profilerHas*OrThrow helper functions.

    const st = new ShardingTest({shards: 2, mongos: 1, config: 1});

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    const shard0DB = primaryShardDB = st.shard0.getDB(jsTestName());
    const shard1DB = st.shard1.getDB(jsTestName());

    assert.commandWorked(mongosDB.dropDatabase());

    // Enable profiling on each shard to verify that no $mergeCursors occur.
    assert.commandWorked(shard0DB.setProfilingLevel(2));
    assert.commandWorked(shard1DB.setProfilingLevel(2));

    // Always merge pipelines which cannot merge on mongoS on the primary shard instead, so we know
    // where to check for $mergeCursors.
    assert.commandWorked(
        mongosDB.adminCommand({setParameter: 1, internalQueryAlwaysMergeOnPrimaryShard: true}));

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

    // Write 400 documents across the 4 chunks.
    for (let i = -200; i < 200; i++) {
        assert.writeOK(mongosColl.insert({_id: i, a: [i], b: {redactThisDoc: true}, c: true}));
    }

    /**
     * Runs the aggregation specified by 'pipeline', verifying that:
     * - The number of documents returned by the aggregation matches 'expectedCount'.
     * - The merge was performed on a mongoS if 'mergeOnMongoS' is true, and on a shard otherwise.
     */
    function assertMergeBehaviour({testName, pipeline, mergeOnMongoS, expectedCount}) {
        // Verify that the 'mergeOnMongoS' explain() output for this pipeline matches our
        // expectation.
        assert.eq(
            assert.commandWorked(mongosColl.explain().aggregate(pipeline, {comment: testName}))
                .mergeOnMongoS,
            mergeOnMongoS);

        assert.eq(mongosColl.aggregate(pipeline, {comment: testName}).itcount(), expectedCount);

        // Verify that a $mergeCursors aggregation ran on the primary shard if 'mergeOnMongoS' is
        // false, and that no such aggregation ran if 'mergeOnMongoS' is true.
        profilerHasNumMatchingEntriesOrThrow({
            profileDB: primaryShardDB,
            numExpectedMatches: (mergeOnMongoS ? 0 : 1),
            filter: {
                "command.aggregate": mongosColl.getName(),
                "command.comment": testName,
                "command.pipeline.$mergeCursors": {$exists: 1}
            }
        });
    }

    /**
     * Throws an assertion if the aggregation specified by 'pipeline' does not produce
     * 'expectedCount' results, or if the merge phase is not performed on the mongoS.
     */
    function assertMergeOnMongoS({testName, pipeline, expectedCount}) {
        assertMergeBehaviour({
            testName: testName,
            pipeline: pipeline,
            mergeOnMongoS: true,
            expectedCount: expectedCount
        });
    }

    /**
     * Throws an assertion if the aggregation specified by 'pipeline' does not produce
     * 'expectedCount' results, or if the merge phase was not performed on a shard.
     */
    function assertMergeOnMongoD({testName, pipeline, expectedCount}) {
        assertMergeBehaviour({
            testName: testName,
            pipeline: pipeline,
            mergeOnMongoS: false,
            expectedCount: expectedCount
        });
    }

    //
    // Test cases.
    //

    let testName;

    // Test that a $match pipeline with an empty merge stage is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_match_only",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}],
        expectedCount: 400
    });

    // Test that a $sort stage which merges pre-sorted streams is run on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_sort_presorted",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sort: {_id: -1}}],
        expectedCount: 400
    });

    // Test that a $sort stage which must sort the dataset from scratch is NOT run on mongoS.
    assertMergeOnMongoD({
        testName: "agg_mongos_merge_sort_in_mem",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sort: {_id: -1}}, {$sort: {a: 1}}],
        expectedCount: 400
    });

    // Test that $skip is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_skip",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sort: {_id: -1}}, {$skip: 100}],
        expectedCount: 300
    });

    // Test that $limit is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_limit",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$limit: 50}],
        expectedCount: 50
    });

    // Test that $sample is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_sample",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sample: {size: 50}}],
        expectedCount: 50
    });

    // Test that merge pipelines containing all mongos-runnable stages produce the expected output.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_all_mongos_runnable_stages",
        pipeline: [
            {$match: {_id: {$gte: -5, $lte: 100}}},
            {$sort: {_id: -1}},
            {$skip: 95},
            {$limit: 10},
            {$addFields: {d: true}},
            {$unwind: "$a"},
            {$sample: {size: 5}},
            {$project: {c: 0}},
            {
              $redact: {
                  $cond:
                      {if: {$eq: ["$redactThisDoc", true]}, then: "$$PRUNE", else: "$$DESCEND"}
              }
            },
            {
              $match: {
                  _id: {$gte: -4, $lte: 5},
                  a: {$gte: -4, $lte: 5},
                  b: {$exists: false},
                  c: {$exists: false},
                  d: true
              }
            }
        ],
        expectedCount: 5
    });
})();