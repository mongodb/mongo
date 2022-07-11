/**
 * Tests that split aggregations whose merge pipelines are eligible to run on mongoS do so, and
 * produce the expected results. Stages which are eligible to merge on mongoS include:
 *
 * - Splitpoints whose merge components are non-blocking, e.g. $skip, $limit, $sort, $sample.
 * - Non-splittable streaming stages, e.g. $match, $project, $unwind.
 * - Blocking stages in cases where 'allowDiskUse' is false, e.g. $group, $bucketAuto.
 *
 * @tags: [
 *   requires_profiling,
 * ]
 */

(function() {
load("jstests/libs/profiler.js");         // For profilerHas*OrThrow helper functions.
load('jstests/libs/geo_near_random.js');  // For GeoNearRandomTest.
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findDataBearingNodes.

const st = new ShardingTest({shards: 2, mongos: 1});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];
const unshardedColl = mongosDB[jsTestName() + "_unsharded"];
const primaryShardDB = st.shard0.getDB(jsTestName());

assert.commandWorked(mongosDB.dropDatabase());

// Always merge pipelines which cannot merge on mongoS on the primary shard instead, so we know
// where to check for $mergeCursors.
assert.commandWorked(
    mongosDB.adminCommand({setParameter: 1, internalQueryAlwaysMergeOnPrimaryShard: true}));

// Enable sharding on the test DB and ensure its primary is shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

// Shard the test collection on _id.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// We will need to test $geoNear on this collection, so create a 2dsphere index.
assert.commandWorked(mongosColl.createIndex({geo: "2dsphere"}));

// We will test that $textScore metadata is not propagated to the user, so create a text index.
assert.commandWorked(mongosColl.createIndex({text: "text"}));

// Split the collection into 4 chunks: [MinKey, -100), [-100, 0), [0, 100), [100, MaxKey).
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: -100}}));
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 100}}));

// Move the [0, 100) and [100, MaxKey) chunks to shard1.
assert.commandWorked(mongosDB.adminCommand(
    {moveChunk: mongosColl.getFullName(), find: {_id: 50}, to: st.shard1.shardName}));
assert.commandWorked(mongosDB.adminCommand(
    {moveChunk: mongosColl.getFullName(), find: {_id: 150}, to: st.shard1.shardName}));

// Create a random geo co-ord generator for testing.
var georng = new GeoNearRandomTest(mongosColl, mongosDB);

// Write 400 documents across the 4 chunks.
for (let i = -200; i < 200; i++) {
    assert.commandWorked(mongosColl.insert(
        {_id: i, a: [i], b: {redactThisDoc: true}, c: true, geo: georng.mkPt(), text: "txt"}));
    assert.commandWorked(unshardedColl.insert({_id: i, x: i}));
}

let testNameHistory = new Set();

// Clears system.profile and restarts the profiler on the primary shard. We enable profiling to
// verify that no $mergeCursors occur during tests where we expect the merge to run on mongoS.
function startProfiling() {
    assert.commandWorked(primaryShardDB.setProfilingLevel(0));
    primaryShardDB.system.profile.drop();
    assert.commandWorked(primaryShardDB.setProfilingLevel(2));
}

/**
 * Runs the aggregation specified by 'pipeline', verifying that:
 * - The number of documents returned by the aggregation matches 'expectedCount'.
 * - The merge was performed on a mongoS if 'mergeType' is 'mongos', and on a shard otherwise.
 */
function assertMergeBehaviour(
    {testName, pipeline, mergeType, batchSize, allowDiskUse, expectedCount}) {
    // Ensure that this test has a unique name.
    assert(!testNameHistory.has(testName));
    testNameHistory.add(testName);

    // Create the aggregation options from the given arguments.
    const opts = {
        comment: testName,
        cursor: (batchSize ? {batchSize: batchSize} : {}),
    };

    if (allowDiskUse !== undefined) {
        opts.allowDiskUse = allowDiskUse;
    }

    // Verify that the explain() output's 'mergeType' field matches our expectation.
    assert.eq(
        assert.commandWorked(mongosColl.explain().aggregate(pipeline, Object.extend({}, opts)))
            .mergeType,
        mergeType);

    // Verify that the aggregation returns the expected number of results.
    assert.eq(mongosColl.aggregate(pipeline, opts).itcount(), expectedCount);

    // Verify that a $mergeCursors aggregation ran on the primary shard if 'mergeType' is not
    // 'mongos', and that no such aggregation ran otherwise.
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: primaryShardDB,
        numExpectedMatches: (mergeType === "mongos" ? 0 : 1),
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
function assertMergeOnMongoS({testName, pipeline, batchSize, allowDiskUse, expectedCount}) {
    assertMergeBehaviour({
        testName: testName,
        pipeline: pipeline,
        mergeType: "mongos",
        batchSize: (batchSize || 10),
        allowDiskUse: allowDiskUse,
        expectedCount: expectedCount
    });
}

/**
 * Throws an assertion if the aggregation specified by 'pipeline' does not produce
 * 'expectedCount' results, or if the merge phase was not performed on a shard.
 */
function assertMergeOnMongoD(
    {testName, pipeline, mergeType, batchSize, allowDiskUse, expectedCount}) {
    assertMergeBehaviour({
        testName: testName,
        pipeline: pipeline,
        mergeType: (mergeType || "anyShard"),
        batchSize: (batchSize || 10),
        allowDiskUse: allowDiskUse,
        expectedCount: expectedCount
    });
}

/**
 * Runs a series of test cases which will consistently merge on mongoS or mongoD regardless of
 * whether 'allowDiskUse' is true, false or omitted.
 */
function runTestCasesWhoseMergeLocationIsConsistentRegardlessOfAllowDiskUse(allowDiskUse) {
    // Test that a $match pipeline with an empty merge stage is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_match_only",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}],
        allowDiskUse: allowDiskUse,
        expectedCount: 400
    });

    // Test that a $sort stage which merges pre-sorted streams is run on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_sort_presorted",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sort: {_id: -1}}],
        allowDiskUse: allowDiskUse,
        expectedCount: 400
    });

    // Test that $skip is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_skip",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sort: {_id: -1}}, {$skip: 300}],
        allowDiskUse: allowDiskUse,
        expectedCount: 100
    });

    // Test that $limit is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_limit",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$limit: 300}],
        allowDiskUse: allowDiskUse,
        expectedCount: 300
    });

    // Test that $sample is merged on mongoS if it is the splitpoint, since this will result in
    // a merging $sort of presorted streams in the merge pipeline.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_sample_splitpoint",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sample: {size: 300}}],
        allowDiskUse: allowDiskUse,
        expectedCount: 300
    });

    // Test that $geoNear is merged on mongoS.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_geo_near",
        pipeline:
            [{$geoNear: {near: [0, 0], distanceField: "distance", spherical: true}}, {$limit: 300}],
        allowDiskUse: allowDiskUse,
        expectedCount: 300
    });

    // Test that $facet is merged on mongoS if all pipelines are mongoS-mergeable regardless of
    // 'allowDiskUse'.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_facet_all_pipes_eligible_for_mongos",
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {
                $facet: {
                    pipe1: [{$match: {_id: {$gt: 0}}}, {$skip: 10}, {$limit: 150}],
                    pipe2: [{$match: {_id: {$lt: 0}}}, {$project: {_id: 0, a: 1}}]
                }
            }
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 1
    });

    // Test that $facet is merged on mongoD if any pipeline requires a primary shard merge,
    // regardless of 'allowDiskUse'.
    assertMergeOnMongoD({
            testName: "agg_mongos_merge_facet_pipe_needs_primary_shard_disk_use_" + allowDiskUse,
            pipeline: [
                {$match: {_id: {$gte: -200, $lte: 200}}},
                {
                  $facet: {
                      pipe1: [{$match: {_id: {$gt: 0}}}, {$skip: 10}, {$limit: 150}],
                      pipe2: [
                          {$match: {_id: {$lt: 0}}},
                          {
                            $lookup: {
                                from: unshardedColl.getName(),
                                localField: "_id",
                                foreignField: "_id",
                                as: "lookupField"
                            }
                          }
                      ]
                  }
                }
            ],
            mergeType: "primaryShard",
            allowDiskUse: allowDiskUse,
            expectedCount: 1
        });

    // Test that a pipeline whose merging half can be run on mongos using only the mongos
    // execution machinery returns the correct results.
    // TODO SERVER-30882 Find a way to assert that all stages get absorbed by mongos.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_all_mongos_runnable_skip_and_limit_stages",
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {$sort: {_id: -1}},
            {$skip: 150},
            {$limit: 150},
            {$skip: 5},
            {$limit: 1},
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 1
    });

    // Test that a merge pipeline which needs to run on a shard is NOT merged on mongoS
    // regardless of 'allowDiskUse'.
    assertMergeOnMongoD({
        testName: "agg_mongos_merge_primary_shard_disk_use_" + allowDiskUse,
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {$_internalSplitPipeline: {mergeType: "anyShard"}}
        ],
        mergeType: "anyShard",
        allowDiskUse: allowDiskUse,
        expectedCount: 400
    });

    // Test that equality $lookup continues to be merged on the primary shard when the foreign
    // collection is unsharded.
    assertMergeOnMongoD({
        testName: "agg_mongos_merge_lookup_unsharded_flag_on_disk_use_" + allowDiskUse,
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            // $lookup is now allowed on the shards pipeline, but we should force it to be part
            // of the merge pipeline for this test.
            {$_internalSplitPipeline: {}},
            {
            $lookup: {
                from: unshardedColl.getName(),
                localField: "_id",
                foreignField: "_id",
                as: "lookupField"
            }
            }
        ],
        mergeType: "primaryShard",
        allowDiskUse: allowDiskUse,
        expectedCount: 400
    });

    // Test that equality $lookup is merged on mongoS when the foreign collection is sharded.
    assertMergeOnMongoS({
        testName: "agg_mongos_merge_lookup_sharded_flag_on_disk_use_" + allowDiskUse,
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            // $lookup is now allowed on the shards pipeline, but we should force it to be part
            // of the merge pipeline for this test.
            {$_internalSplitPipeline: {}},
            {
            $lookup: {
                from: mongosColl.getName(),
                localField: "_id",
                foreignField: "_id",
                as: "lookupField"
            }
            }
        ],
        mergeType: "mongos",
        allowDiskUse: allowDiskUse,
        expectedCount: 400
    });
}

/**
 * Runs a series of test cases which will always merge on mongoD when 'allowDiskUse' is true,
 * and on mongoS when 'allowDiskUse' is false or omitted.
 */
function runTestCasesWhoseMergeLocationDependsOnAllowDiskUse(allowDiskUse) {
    // All test cases should merge on mongoD if allowDiskUse is true, mongoS otherwise.
    const assertMergeOnMongoX = (allowDiskUse ? assertMergeOnMongoD : assertMergeOnMongoS);

    // Test that $group is only merged on mongoS if 'allowDiskUse' is not set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_group_allow_disk_use",
        pipeline:
            [{$match: {_id: {$gte: -200, $lte: 200}}}, {$group: {_id: {$mod: ["$_id", 150]}}}],
        allowDiskUse: allowDiskUse,
        expectedCount: 299
    });

    // Adjacent $sort stages will be coalesced and merge sort will occur on anyShard when disk use
    // is allowed, and on mongos otherwise.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_blocking_sort_allow_disk_use",
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {$sort: {_id: 1}},
            {$_internalSplitPipeline: {}},
            {$sort: {a: 1}}
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 400
    });

    // Test that a blocking $sample is only merged on mongoS if 'allowDiskUse' is not set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_blocking_sample_allow_disk_use",
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {$sample: {size: 300}},
            {$sample: {size: 200}}
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 200
    });

    // Test that $facet is only merged on mongoS if all pipelines are mongoS-mergeable when
    // 'allowDiskUse' is not set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_facet_allow_disk_use",
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {
                $facet: {
                    pipe1: [{$match: {_id: {$gt: 0}}}, {$skip: 10}, {$limit: 150}],
                    pipe2: [{$match: {_id: {$lt: 0}}}, {$sort: {a: -1}}]
                }
            }
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 1
    });

    // Test that $bucketAuto is only merged on mongoS if 'allowDiskUse' is not set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_bucket_auto_allow_disk_use",
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {$bucketAuto: {groupBy: "$_id", buckets: 10}}
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 10
    });

    // Test that $lookup with a subpipeline containing a spillable stage is only merged on
    // mongoS when 'allowDiskUse' is not set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_lookup_subpipeline_disk_use_" + allowDiskUse,
        pipeline: [
            {$match: {_id: {$gt: -25, $lte: 25}}},
            // $lookup is now allowed on the shards pipeline, but we should force it to be part
            // of the merge pipeline for this test.
            {$_internalSplitPipeline: {}},
            {
                $lookup: {
                    from: mongosColl.getName(),
                    let: {idVar: "$_id"},
                    pipeline: [{$group: {_id: {$mod: ["$_id", 150]}}}],
                    as: "lookupField"
                }
            }
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 50
    });

    //
    // Test composite stages.
    //

    // Test that $bucket ($group->$sort) is merged on mongoS iff 'allowDiskUse' is not set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_bucket_allow_disk_use",
        pipeline: [
            {$match: {_id: {$gte: -200, $lte: 200}}},
            {$bucket: {groupBy: "$_id", boundaries: [-200, -150, -100, -50, 0, 50, 100, 150, 200]}}
        ],
        allowDiskUse: allowDiskUse,
        expectedCount: 8
    });

    // Test that $sortByCount ($group->$sort) is merged on mongoS iff 'allowDiskUse' isn't set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_sort_by_count_allow_disk_use",
        pipeline: [{$match: {_id: {$gte: -200, $lte: 200}}}, {$sortByCount: {$mod: ["$_id", 150]}}],
        allowDiskUse: allowDiskUse,
        expectedCount: 299
    });

    // Test that $count ($group->$project) is merged on mongoS iff 'allowDiskUse' is not set.
    assertMergeOnMongoX({
        testName: "agg_mongos_merge_count_allow_disk_use",
        pipeline: [{$match: {_id: {$gte: -150, $lte: 1500}}}, {$count: "doc_count"}],
        allowDiskUse: allowDiskUse,
        expectedCount: 1
    });
}

// Run all test cases for each potential value of 'allowDiskUse'.
for (let allowDiskUse of [false, undefined, true]) {
    // Reset the profiler and clear the list of tests that ran on the previous iteration.
    testNameHistory.clear();
    startProfiling();

    // Run all test cases.
    runTestCasesWhoseMergeLocationIsConsistentRegardlessOfAllowDiskUse(allowDiskUse);
    runTestCasesWhoseMergeLocationDependsOnAllowDiskUse(allowDiskUse);
}

// Start a new profiling session before running the final few tests.
startProfiling();

// Test that merge pipelines containing all mongos-runnable stages produce the expected output.
assertMergeOnMongoS({
    testName: "agg_mongos_merge_all_mongos_runnable_stages",
    pipeline: [
        {$geoNear: {near: [0, 0], distanceField: "distance", spherical: true}},
        {$sort: {a: 1}},
        {$skip: 150},
        {$limit: 150},
        {$addFields: {d: true}},
        {$unwind: "$a"},
        {$sample: {size: 100}},
        {$project: {c: 0, geo: 0, distance: 0}},
        {$group: {_id: "$_id", doc: {$push: "$$CURRENT"}}},
        {$unwind: "$doc"},
        {$replaceRoot: {newRoot: "$doc"}},
        {$facet: {facetPipe: [{$match: {_id: {$gte: -200, $lte: 200}}}]}},
        {$unwind: "$facetPipe"},
        {$replaceRoot: {newRoot: "$facetPipe"}},
        {
            $redact:
                {$cond: {if: {$eq: ["$redactThisDoc", true]}, then: "$$PRUNE", else: "$$DESCEND"}}
        },
        {
            $match: {
                _id: {$gte: -50, $lte: 100},
                a: {$type: "number", $gte: -50, $lte: 100},
                b: {$exists: false},
                c: {$exists: false},
                d: true,
                geo: {$exists: false},
                distance: {$exists: false},
                text: "txt"
            }
        }
    ],
    expectedCount: 100
});

// Test that metadata is not propagated to the user when a pipeline which produces metadata
// fields merges on mongoS.
const metaDataTests = [
    {pipeline: [{$sort: {_id: -1}}], verifyNoMetaData: (doc) => assert.isnull(doc.$sortKey)},
    {
        pipeline: [{$match: {$text: {$search: "txt"}}}],
        verifyNoMetaData: (doc) => assert.isnull(doc.$textScore)
    },
    {pipeline: [{$sample: {size: 300}}], verifyNoMetaData: (doc) => assert.isnull(doc.$randVal)},
    {
        pipeline: [{$match: {$text: {$search: "txt"}}}, {$sort: {text: 1}}],
        verifyNoMetaData: (doc) =>
            assert.docEq([doc.$textScore, doc.$sortKey], [undefined, undefined])
    }
];

for (let metaDataTest of metaDataTests) {
    assert.gte(mongosColl.aggregate(metaDataTest.pipeline).itcount(), 300);
    mongosColl.aggregate(metaDataTest.pipeline).forEach(metaDataTest.verifyNoMetaData);
}

st.stop();
})();
