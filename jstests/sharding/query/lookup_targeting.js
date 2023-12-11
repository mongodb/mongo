/*
 * Test that the targeting of $lookup queries and any sub-queries works correctly.
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsOnShardingCatalog,
 *   featureFlagMoveCollection,
 *   assumes_balancer_off,
 *   requires_sharding,
 *   requires_spawning_own_processes,
 *   requires_profiling,
 * ]
 */

import {getAggPlanStage} from "jstests/libs/analyze_plan.js";
import {profilerHasAtLeastOneMatchingEntryOrThrow} from "jstests/libs/profiler.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const kDbName = "lookup_targeting";
const st = new ShardingTest({shards: 3});
const db = st.s.getDB(kDbName);
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard2 = st.shard2.shardName;
const shard0DB = st.shard0.getDB(kDbName);
const shard1DB = st.shard1.getDB(kDbName);
const shard2DB = st.shard2.getDB(kDbName);

// Utility to clear the profiling collection.
function resetProfiling() {
    for (const shardDB of [shard0DB, shard1DB, shard2DB]) {
        assert.commandWorked(shardDB.setProfilingLevel(0));
        shardDB.system.profile.drop();
        assert.commandWorked(shardDB.setProfilingLevel(2));
    }
}

resetProfiling();

assert.commandWorked(db.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

function setupColl({collName, indexList, docs, collType, shardKey, chunkList, owningShard}) {
    const coll = db[collName];
    if (indexList && indexList.length > 0) {
        assert.commandWorked(coll.createIndexes(indexList));
    }

    if (collType === "sharded") {
        CreateShardedCollectionUtil.shardCollectionWithChunks(coll, shardKey, chunkList);
    } else if (collType == "unsplittable") {
        assert.commandWorked(
            db.runCommand({createUnsplittableCollection: collName, dataShard: owningShard}));
    } else {
        assert(false, "Unknown collection type " + tojson(collType));
    }

    assert.commandWorked(coll.insertMany(docs));
}

// Create two sharded collections, both of which are distributed across the 3 shards.
const kShardedColl1Name = "sharded1";
const kShardedColl1Docs = [
    {_id: 0, a: -1},
    {_id: 1, a: 1},
    {_id: 2, a: 101},
];
const kShardedColl1ChunkList = [
    {min: {a: MinKey}, max: {a: 0}, shard: shard0},
    {min: {a: 0}, max: {a: 100}, shard: shard1},
    {min: {a: 100}, max: {a: MaxKey}, shard: shard2}
];
setupColl({
    collName: kShardedColl1Name,
    indexList: [{a: 1}],
    docs: kShardedColl1Docs,
    collType: "sharded",
    shardKey: {a: 1},
    chunkList: kShardedColl1ChunkList
});

const kShardedColl2Name = "sharded2";
const kShardedColl2Docs = [
    {_id: 0, b: -1},
    {_id: 1, b: 1},
    {_id: 2, b: 101},
];
const kShardedColl2ChunkList = [
    {min: {b: MinKey}, max: {b: 0}, shard: shard0},
    {min: {b: 0}, max: {b: 100}, shard: shard1},
    {min: {b: 100}, max: {b: MaxKey}, shard: shard2}
];
setupColl({
    collName: kShardedColl2Name,
    indexList: [{b: 1}],
    docs: kShardedColl2Docs,
    collType: "sharded",
    shardKey: {b: 1},
    chunkList: kShardedColl2ChunkList
});

// Create three unsplittable collections, two of which are on the same shard.
const kUnsplittable1CollName = "unsplittable_1";
const kUnsplittable1Docs = [
    {_id: 0, a: -1, unsplittable: 1},
    {_id: 1, a: 1, unsplittable: 1},
    {_id: 2, a: 101, unsplittable: 1}
];
setupColl({
    collName: kUnsplittable1CollName,
    docs: kUnsplittable1Docs,
    collType: "unsplittable",
    owningShard: shard1,
});
const kUnsplittable2CollName = "unsplittable_2";
const kUnsplittable2Docs = [
    {_id: 0, a: -1, unsplittable: 2},
    {_id: 1, a: 1, unsplittable: 2},
    {_id: 2, a: 101, unsplittable: 2}
];
setupColl({
    collName: kUnsplittable2CollName,
    docs: kUnsplittable2Docs,
    collType: "unsplittable",
    owningShard: shard2,
});

const kUnsplittable3CollName = "unsplittable_3_collocated_with_2";
const kUnsplittable3Docs = [
    {_id: 0, a: -1, unsplittable: 3},
    {_id: 1, a: 1, unsplittable: 3},
    {_id: 2, a: 101, unsplittable: 3}
];
setupColl({
    collName: kUnsplittable3CollName,
    docs: kUnsplittable3Docs,
    collType: "unsplittable",
    owningShard: shard2,
});

/**
 * Utility which asserts that the aggregation stages in 'actualStages' match those in
 * 'expectedStages'.
 */
function assertExpectedStages(expectedStages, actualStages, explain) {
    assert.eq(expectedStages.length, actualStages.length, explain);
    let stageIdx = 0;
    for (const stage of expectedStages) {
        const spec = actualStages[stageIdx];
        assert(spec.hasOwnProperty(stage),
               "Expected stage " + tojson(stage) + " in explain " + tojson(explain));
        stageIdx++;
    }
}

/**
 * Utility to create a filter for querying the database profiler with the provided parameters.
 */
function createProfileFilter({ns, comment, expectedStages}) {
    let profileFilter = {"op": "command", "command.aggregate": ns};
    if (comment) {
        profileFilter["command.comment"] = comment;
    }
    let idx = 0;
    for (const stage of expectedStages) {
        const fieldName = "command.pipeline." + idx + "." + stage;
        profileFilter[fieldName] = {"$exists": true};
        idx++;
    }

    // If we have an empty stage, this may indicate a cursor established by a $mergeCursors stage
    // internally. As such, we wish to verify that said cursor was established, and had no pipeline
    // stages.
    if (expectedStages && expectedStages.length === 0) {
        profileFilter["command.pipeline"] = {"$size": 0};
    }
    return profileFilter;
}

/**
 * Utility which makes certain assertions about 'explain' (obtained by running explain), namely:
 * - 'expectedMergingShard' and 'expectedMergingStages' allow for assertions around the shard which
 * was chosen as the merger and what pipeline is used to merge.
 * - 'expectedShard' and 'expectedShardStages' allow for assertions around targeting a single shard
 * for execution.
 * - 'assertSBELookupPushdown' asserts that $lookup was pushed down into SBE when present.
 */
function assertExplainLookupTargeting(explain, {
    expectedMergingShard,
    expectedMergingStages,
    expectedShard,
    expectedShardStages,
    assertSBELookupPushdown
}) {
    if (expectedMergingShard) {
        assert.eq(explain.mergeType, "specificShard", explain);
        assert.eq(explain.mergeShardId, expectedMergingShard, explain);
        assert(explain.hasOwnProperty("splitPipeline"), explain);
        const split = explain.splitPipeline;
        assert(split.hasOwnProperty("mergerPart"), explain);
        const mergerPart = split.mergerPart;
        assert(expectedMergingStages, explain);
        assertExpectedStages(expectedMergingStages, mergerPart, explain);
    } else {
        assert.neq(explain.mergeType,
                   "specificShard",
                   "Expected not to merge on a specific shard; explain " + tojson(explain));
        assert.neq(explain.mergeType, "anyShard", "Expected not to merge on any shard", explain);
    }

    if (expectedShard) {
        assert(explain.hasOwnProperty("shards"), explain);
        const shards = explain.shards;
        const keys = Object.keys(shards);
        assert.eq(keys.length, 1, explain);
        assert.eq(expectedShard, keys[0], explain);

        const shard = shards[expectedShard];
        if (expectedShardStages) {
            const stages = shard.stages;
            assert(stages, explain);
            assertExpectedStages(expectedShardStages, stages, explain);
        }

        if (assertSBELookupPushdown) {
            const stage = getAggPlanStage(shard, "EQ_LOOKUP", true /* useQueryPlannerSection */);
            assert.neq(stage, null, shard);
        }
    }
}

// Map from shard name to database connection. Used when determine which shard to read from when
// gathering profiler output.
const shardProfileDBMap = {
    [shard0]: shard0DB,
    [shard1]: shard1DB,
    [shard2]: shard2DB
};

/**
 * Helper function which runs 'pipeline' using the explain and aggregate commands to not only verify
 * correctness in terms of results, but also in terms of shard targeting.
 * - 'targetCollName' names the collection to target 'pipeline' with.
 * - 'explainAssertionObj' describes assertions to be made against the explain output (see
 * 'assertExplainLookupTargeting' for more detail).
 * - 'expectedResults' contains the output that running 'pipeline' should produce.
 * - 'comment' is a string that will allow 'pipeline' (and, in some cases, sub-queries) to be
 * uniquely identified in profiler output.
 * - 'profileFilters' is a map from shard name to objects containing arguments to create a filter to
 * query the profiler output.
 */
function assertLookupShardTargeting({
    pipeline,
    targetCollName,
    explainAssertionObj,
    expectedResults,
    comment,
    profileFilters,
}) {
    const coll = db[targetCollName];
    if (explainAssertionObj) {
        const explain = coll.explain().aggregate(pipeline);
        assertExplainLookupTargeting(explain, explainAssertionObj);
    }
    // Always reset profiling before running an aggregate.
    resetProfiling();

    const options = comment ? {'comment': comment} : {};
    const res = coll.aggregate(pipeline, options).toArray();
    assert.sameMembers(res, expectedResults, "$lookup results did not match");

    if (profileFilters) {
        for (const [shard, filterList] of Object.entries(profileFilters)) {
            const profileDB = shardProfileDBMap[shard];
            assert.neq(profileDB, null);
            for (let filter of filterList) {
                filter.comment = comment;
                profilerHasAtLeastOneMatchingEntryOrThrow(
                    {profileDB: profileDB, filter: createProfileFilter(filter)});
            }
        }
    }
}

// Inner collection is unsplittable and not on the primary shard. Outer collection is sharded.
// In this case, we should be merging on the shard which owns the unsplittable collection.
assertLookupShardTargeting({
    pipeline:
        [{$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kShardedColl1Name,
    explainAssertionObj:
        {expectedMergingShard: shard1, expectedMergingStages: ["$mergeCursors", "$lookup"]},
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 1}]},
        {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1}]},
        {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 1}]},
    ],
    comment: "outer_sharded_inner_unsplittable",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
        [shard1]: [
            /**
             * TODO SERVER-81335: The cursor against 'kShardedColl1Name' may no longer be profiled
             * once the $mergeCursors pipeline can execute it locally. If this is the case, remove
             * this assertion (here and elsewhere).
             */
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}
        ],
        [shard2]: [{ns: kShardedColl1Name, expectedStages: []}],
    }
});

// Outer collection is unsplittable and not on the primary shard. Inner collection is sharded.
// We should target the shard which owns the unsplittable collection.
assertLookupShardTargeting({
    pipeline: [{$lookup: {from: kShardedColl1Name, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {expectedShard: shard1, expectedShardStages: ["$cursor", "$lookup"]},
    expectedResults: [
        {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1}]},
        {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1}]},
        {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101}]},
    ],
    comment: "outer_unsplittable_inner_sharded",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
        [shard1]: [{ns: kUnsplittable1CollName, expectedStages: ["$lookup"]}],
        [shard2]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
    },
});

// Both collections are unsplittable and are located on different shards. We should merge on the
// shard which owns the inner collection in each case.
assertLookupShardTargeting({
    pipeline:
        [{$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
        expectedShard: shard1,
    },
    expectedResults: [
        {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]},
        {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]},
        {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]},
    ],
    comment: "outer_unsplittable_1_inner_unsplittable_2",
    profileFilters: {
        [shard1]: [{ns: kUnsplittable1CollName, expectedStages: []}],
        [shard2]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$lookup"]}]
    },
});

assertLookupShardTargeting({
    pipeline:
        [{$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable2CollName,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
        expectedShard: shard2,
    },
    expectedResults: [
        {_id: 0, a: -1, unsplittable: 2, out: [{_id: 0, a: -1, unsplittable: 1}]},
        {_id: 1, a: 1, unsplittable: 2, out: [{_id: 1, a: 1, unsplittable: 1}]},
        {_id: 2, a: 101, unsplittable: 2, out: [{_id: 2, a: 101, unsplittable: 1}]},
    ],
    comment: "outer_unsplittable_2_inner_unsplittable_1",
    profileFilters: {
        [shard1]: [{ns: kUnsplittable2CollName, expectedStages: ["$mergeCursors", "$lookup"]}],
        [shard2]: [{ns: kUnsplittable2CollName, expectedStages: []}],
    },
});

// Issue an aggregate featuring a $lookup whose inner collection is a view over an unsplittable
// collection.
// TODO SERVER-83902: We currently assert that we merge on the primary shard. Ideally, we should
// merge on shard2, the node which owns the underlying collection. Since views are not currently
// tracked in the sharding catalog, we default to the primary shard because there does not exist a
// routing table entry for our view. Though there will be an entry in the routing table for the
// underlying collection, we will not resolve the view until we issue an aggregate for the inner
// side (by which point we've already created a distributed plan for this aggregation). Fix this so
// that we pick shard2 as the merging node.
const kViewName = "view_over_unsplittable_2";
assert.commandWorked(db.createView(
    kViewName /* viewName */, kUnsplittable2CollName /* viewOn */, [] /* pipeline */));
assertLookupShardTargeting({
    pipeline: [{$lookup: {from: kViewName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {
        expectedMergingShard: shard0,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
        expectedShard: shard1,
    },
    expectedResults: [
        {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]},
        {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]},
        {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]},
    ],
    comment: "lookup_inner_side_targets_view_over_unsplittable_2",
    profileFilters: {
        [shard0]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$lookup"]}],
        [shard1]: [{ns: kUnsplittable1CollName, expectedStages: []}],
        [shard2]: [{ns: kUnsplittable2CollName, expectedStages: ["$match"]}],
    },
});

// Clean up the view.
assert(db[kViewName].drop());

if (checkSBEEnabled(db)) {
    // Both collections are unsplittable and are collocated on the same shard. Test that we can do
    // SBE $lookup pushdown, regardless of which collection is on the inner side.
    assertLookupShardTargeting({
        pipeline: [
            {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}}
        ],
        targetCollName: kUnsplittable3CollName,
        explainAssertionObj: {
            expectedShard: shard2,
            assertSBELookupPushdown: true,
        },
        expectedResults: [
            {_id: 0, a: -1, unsplittable: 3, out: [{_id: 0, a: -1, unsplittable: 2}]},
            {_id: 1, a: 1, unsplittable: 3, out: [{_id: 1, a: 1, unsplittable: 2}]},
            {_id: 2, a: 101, unsplittable: 3, out: [{_id: 2, a: 101, unsplittable: 2}]},
        ],
        comment: "lookup_sbe_pushdown_target_unsplittable_3",
    });

    assertLookupShardTargeting({
        pipeline: [
            {$lookup: {from: kUnsplittable3CollName, localField: "a", foreignField: "a", as: "out"}}
        ],
        targetCollName: kUnsplittable2CollName,
        explainAssertionObj: {
            expectedShard: shard2,
            assertSBELookupPushdown: true,
        },
        expectedResults: [
            {_id: 0, a: -1, unsplittable: 2, out: [{_id: 0, a: -1, unsplittable: 3}]},
            {_id: 1, a: 1, unsplittable: 2, out: [{_id: 1, a: 1, unsplittable: 3}]},
            {_id: 2, a: 101, unsplittable: 2, out: [{_id: 2, a: 101, unsplittable: 3}]},
        ],
        comment: "lookup_sbe_pushdown_target_unsplittable_2",
    });
}

// Verify the targeting behavior of $facet. In particular, we should always merge or target the
// shard corresponding to the first inner unsplittable collection among the $lookup facet pipelines.
// TODO SERVER-79581: Update this test case and the one below to not use a leading $match. This is
// done to avoid an unordered result set and the need to introduce a $sort. This can be fixed if the
// 'expectedResults' function uses 'assertSameMembers', which SERVER-79581 does.
assertLookupShardTargeting({
    pipeline: [
        {$match: {a: -1}},
        {$facet: {
            pipe1: [{
                $lookup:
                    {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out_1"}
            }],
            pipe2: [{
                $lookup:
                    {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out_2"}
            }]
        }
    }],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$facet"]
    },
    expectedResults: [
        {
            pipe1: [
                {_id: 0, a: -1, out_1: [{_id: 0, a: -1, unsplittable: 1}]},
            ],
            pipe2: [
                {_id: 0, a: -1, out_2: [ {_id: 0, a: -1, unsplittable: 2}]},
            ]
        },
    ],
    comment: "facet_lookup_pipeline_merge_on_shard_1",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
        [shard1]: [{ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$facet"]}],
        [shard2]: [{ns: kUnsplittable2CollName, expectedStages: ["$match"]}],
    },
});

assertLookupShardTargeting({
    pipeline: [
        {$match: {a: -1}},
        {$facet: {
            pipe1: [{
                $lookup:
                    {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out_2"}
            }],
            pipe2: [{
                $lookup:
                    {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out_1"}
            }]
        }
    }],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$facet"]
    },
    expectedResults: [
        {
            pipe1: [
                {_id: 0, a: -1, out_2: [{_id: 0, a: -1, unsplittable: 2}]},
            ],
            pipe2: [
                {_id: 0, a: -1, out_1: [{_id: 0, a: -1, unsplittable: 1}]},
            ],
        },
    ],
    comment: "facet_lookup_pipeline_merge_on_shard_2",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
        [shard1]: [{ns: kUnsplittable1CollName, expectedStages: ["$match"]}],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$facet"]}],
    },
});

// If our initial query targets a single shard and that shard is the same as the merging shard, then
// we can target the facet to a single shard (even if we need to issue remotes reads for the inner
// side of the first $lookup).
assertLookupShardTargeting({
    pipeline: [
        {$match: {a: 1}},
        {$facet: {
            pipe1:[{
                $lookup:
                    {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out_1"}
            }],
            pipe2: [{
                $lookup:
                    {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out_2"}
            }],
        }
    }],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedShard: shard1,
        expectedShardStages: ["$cursor", "$facet"]
    },
    expectedResults: [
        {
            pipe1: [
                {_id: 1, a: 1, out_1: [{_id: 1, a: 1, unsplittable: 1}]},
            ],
            pipe2: [
                {_id: 1, a: 1, out_2: [{_id: 1, a: 1, unsplittable: 2}]},
            ],
        },
    ],
    comment: "facet_lookup_pipeline_target_single_shard",
    profileFilters: {
        [shard0]: [],
        [shard1]: [{ns: kShardedColl1Name, expectedStages: ["$match", "$facet"]}],
        [shard2]: [{ns: kUnsplittable2CollName, expectedStages: ["$match"]}],
    },
});

// Issue an aggregate featuring two $lookup stages, where both stages' inner collections are
// unsplittable and reside on different shards. We should always merge on the shard which owns the
// first inner collection.
assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"],
    },
    expectedResults: [
        {
            _id: 0,
            a: -1,
            out: [{_id: 0, a: -1, unsplittable: 1}],
            out_2: [{_id: 0, a: -1, unsplittable: 2}]
        },
        {
            _id: 1,
            a: 1,
            out: [{_id: 1, a: 1, unsplittable: 1}],
            out_2: [{_id: 1, a: 1, unsplittable: 2}]
        },
        {
            _id: 2,
            a: 101,
            out: [{_id: 2, a: 101, unsplittable: 1}],
            out_2: [{_id: 2, a: 101, unsplittable: 2}]
        },
    ],
    comment: "first_lookup_inner_unsplittable_1_second_lookup_inner_unsplittable_2",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup", "$lookup"]}
        ],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kUnsplittable2CollName, expectedStages: ["$match"]}
        ],
    },
});

assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"],
    },

    expectedResults: [
        {
            _id: 0,
            a: -1,
            out: [{_id: 0, a: -1, unsplittable: 2}],
            out_2: [{_id: 0, a: -1, unsplittable: 1}]
        },
        {
            _id: 1,
            a: 1,
            out: [{_id: 1, a: 1, unsplittable: 2}],
            out_2: [{_id: 1, a: 1, unsplittable: 1}]
        },
        {
            _id: 2,
            a: 101,
            out: [{_id: 2, a: 101, unsplittable: 2}],
            out_2: [{_id: 2, a: 101, unsplittable: 1}]
        },
    ],
    comment: "first_lookup_inner_unsplittable_2_second_lookup_inner_unsplittable_1",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kUnsplittable1CollName, expectedStages: ["$match"]}
        ],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup", "$lookup"]}
        ]
    },
});

// Issue aggregates featuring two $lookup stages: one $lookup targets an unsplittable inner
// collection and the other targets a sharded inner collection. We should merge on the shard which
// owns the unsplittable collection.
assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out"}},
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
    },
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, b: -1}], out_2: [{_id: 0, a: -1, unsplittable: 1}]},
        {_id: 1, a: 1, out: [{_id: 1, b: 1}], out_2: [{_id: 1, a: 1, unsplittable: 1}]},
        {_id: 2, a: 101, out: [{_id: 2, b: 101}], out_2: [{_id: 2, a: 101, unsplittable: 1}]},
    ],
    comment: "first_lookup_inner_sharded_second_lookup_inner_unsplittable_1",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$lookup"]}],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}
        ],
        [shard2]: [{ns: kShardedColl1Name, expectedStages: ["$lookup"]}],
    }
});

assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out"}},
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
    },
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, b: -1}], out_2: [{_id: 0, a: -1, unsplittable: 2}]},
        {_id: 1, a: 1, out: [{_id: 1, b: 1}], out_2: [{_id: 1, a: 1, unsplittable: 2}]},
        {_id: 2, a: 101, out: [{_id: 2, b: 101}], out_2: [{_id: 2, a: 101, unsplittable: 2}]},
    ],
    comment: "first_lookup_inner_sharded_second_lookup_inner_unsplittable_2",
    profileFilters: {
        [shard0]: [
            {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
        ],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
        ],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}
        ]
    }
});

// Issue aggregates featuring two $lookup stages, where the first one's inner collection is
// unsplittable and the second one's inner collection is sharded. We should target the shard which
// owns the unsplittable collection.
assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"]
    },
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 1}], out_2: [{_id: 0, b: -1}]},
        {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1}], out_2: [{_id: 1, b: 1}]},
        {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 1}], out_2: [{_id: 2, b: 101}]},
    ],
    comment: "first_lookup_inner_unsplittable_1_second_lookup_inner_sharded",
    profileFilters: {
        [shard0]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl2Name, expectedStages: ["$match"]}
        ],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup", "$lookup"]}
        ],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl2Name, expectedStages: ["$match"]}
        ],
    }
});

assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"]
    },
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 2}], out_2: [{_id: 0, b: -1}]},
        {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 2}], out_2: [{_id: 1, b: 1}]},
        {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 2}], out_2: [{_id: 2, b: 101}]},
    ],
    comment: "first_lookup_inner_unsplittable_2_second_lookup_inner_sharded",
    profileFilters: {
        [shard0]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl2Name, expectedStages: ["$match"]}
        ],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl2Name, expectedStages: ["$match"]}
        ],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup", "$lookup"]}
        ]
    }
});

// Issue an aggregate featuring nested $lookup stages where both inner collections are unsplittable
// and live on different shards. The $lookup should execute on the shard which owns of the first
// inner collection. Then, we should have several $match queries against each of the inner
// collections.
assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
            {$lookup: {from: kUnsplittable2CollName , localField: "a", foreignField: "a", as: "out"}},
        ]}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup"],},
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]}]},
        {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]}]},
        {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]}]},
    ],
    comment: "nested_lookup_inner_unsplittable_1_innermost_unsplittable_2",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]},
            {ns: kUnsplittable1CollName, expectedStages: ["$match"]},
        ],
       [shard2]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kUnsplittable2CollName, expectedStages: ["$match"]},
    ],
    }
});

assertLookupShardTargeting({
    pipeline:
        [
            {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
                {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}},
            ]}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: { expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup"]},
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 2, out: [{_id: 0, a: -1, unsplittable: 1}]}]},
        {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 2, out: [{_id: 1, a: 1, unsplittable: 1}]}]},
        {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 2, out: [{_id: 2, a: 101, unsplittable: 1}]}]},
    ],
    comment: "nested_lookup_inner_unsplittable_2_innermost_unsplittable_1",
    profileFilters: {
        [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
        [shard1]:  [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kUnsplittable1CollName, expectedStages: ["$match"]}],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: []},
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]},
            {ns: kUnsplittable2CollName, expectedStages: ["$match"]}],
       }
});

// Issue an aggregate featuring nested $lookup stages where the innermost collection is
// sharded and the top level $lookup's 'from' collection is unsplittable. We should execute
// on the shard which owns the unsplittable collection.
assertLookupShardTargeting({
    pipeline:
        [
            {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
                {$lookup: {from: kShardedColl2Name , localField: "a", foreignField: "b", as: "out"}},
            ]}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup"]
    },
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, b: -1}]}]},
        {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, b: 1}]}]},
        {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, b: 101}]}]},
    ],
    comment: "nested_lookup_inner_unsplittable_1_innermost_sharded_2",
    profileFilters: {
        [shard0]: [
            {ns: kShardedColl1Name, expectedStages: []}, {ns: kShardedColl2Name, expectedStages: ["$match"]}],
        [shard1]: [{ns: kShardedColl1Name, expectedStages: []}, {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}],
        [shard2]: [{ns: kShardedColl1Name, expectedStages: []}, {ns: kShardedColl2Name, expectedStages: ["$match"]}]},
});

// Issue an aggregate featuring nested $lookup stages where the innermost collection is
// unsplittable, while the top level $lookup's 'from' collection is sharded.
assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out", pipeline: [
            {$lookup: {from: kUnsplittable1CollName, localField: "b", foreignField: "a", as: "out"}},
        ]}}
    ],
    targetCollName: kShardedColl1Name,
    expectedResults: [
        {_id: 0, a: -1, out: [{_id: 0, b: -1, out: [{_id: 0, a: -1, unsplittable: 1}]}]},
        {_id: 1, a: 1, out: [{_id: 1, b: 1, out: [{_id: 1, a: 1, unsplittable: 1}]}]},
        {_id: 2, a: 101, out: [{_id: 2, b: 101, out: [{_id: 2, a: 101, unsplittable: 1}]}]},
    ],
    comment:  "nested_lookup_inner_sharded_innermost_unsplittable_1",
    profileFilters: {
        [shard0]: [
            {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
            {ns: kShardedColl2Name, expectedStages: ["$match"]}],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
            {ns: kUnsplittable1CollName, expectedStages: ["$match"]}],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
            {ns: kShardedColl2Name, expectedStages: ["$match"]}]},
});

// Issue an aggregate featuring three levels of nested $lookups. All involved collections are
// unsplittable, and all live on different shards. However, we expect to merge on the shard which
// owns the top-level inner collection (in this case, shard1).
assert.commandWorked(
    db.adminCommand({moveCollection: db[kUnsplittable3CollName].getFullName(), toShard: shard0}));
assertLookupShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
            {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
                {$lookup: {from: kUnsplittable3CollName, localField: "a", foreignField: "a", as: "out"}},
        ]}}]}}
    ],
    targetCollName: kShardedColl1Name,
    expectedResults: [
        {
            _id: 0,
            a: -1,
            out: [{
                _id: 0,
                a: -1,
                unsplittable: 1,
                out: [{_id: 0, a: -1, unsplittable: 2, out: [{_id: 0, a: -1, unsplittable: 3}]}]
            }]
        },
        {
            _id: 1,
            a: 1,
            out: [{
                _id: 1,
                a: 1,
                unsplittable: 1,
                out: [{_id: 1, a: 1, unsplittable: 2, out: [{_id: 1, a: 1, unsplittable: 3}]}]
            }]
        },
        {
            _id: 2,
            a: 101,
            out: [{
                _id: 2,
                a: 101,
                unsplittable: 1,
                out: [{_id: 2, a: 101, unsplittable: 2, out: [{_id: 2, a: 101, unsplittable: 3}]}]
            }]
        }
    ],
    comment:  "only_test_case_with_three_nested_pipelines",
    profileFilters: {
        [shard0]: [
            {ns: kShardedColl1Name, expectedStages: []}, 
            {ns: kUnsplittable3CollName, expectedStages: ["$match"]}],
        [shard1]: [
            {ns: kShardedColl1Name, expectedStages: []}, 
            {ns: kUnsplittable1CollName, expectedStages: ["$match"]}, 
            {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}],
        [shard2]: [
            {ns: kShardedColl1Name, expectedStages: []}, 
            {ns: kUnsplittable2CollName, expectedStages: ["$match"]}]},
});

// Set of tests which involve moving an unsplittable collection during query execution.

// Test moving the outer collection to another shard during $lookup execution. This should
// result in a 'QueryPlanKilled' error.
let coll = db[kUnsplittable1CollName];

// Add a set of 20 documents to the outer collection which are at least 1 MB in size. This
// makes it so that the documents from the outer collection do not fit in one batch and we
// will have to issue at least one getMore against the outer collection to continue
// constructing the result set.
assert.commandWorked(coll.insertMany(Array(20).fill({a: 1, str: "a".repeat(1024 * 1024)})));

// Establish our cursor. We should not have exhausted our cursor.
const pipeline = [
    {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}},
    {$project: {out: 0}}
];
let cursor = coll.aggregate(pipeline, {batchSize: 1});
assert(cursor.hasNext());

// Move the outer collection to a different shard.
assert.commandWorked(db.adminCommand({moveCollection: coll.getFullName(), toShard: shard0}));

function iterateCursor(c) {
    while (c.hasNext()) {
        c.next();
    }
}

// Subsequent getMore commands should cause our query plan to be killed because to our
// $mergeCursor stage, it will appear as though the outer collection has been dropped.
assert.throwsWithCode(() => iterateCursor(cursor), ErrorCodes.QueryPlanKilled);

// Move the outer collection back to its original shard.
assert.commandWorked(db.adminCommand({moveCollection: coll.getFullName(), toShard: shard1}));

// Test moving the inner collection to another shard during $lookup execution. Because the
// move happens in between executions of the inner pipeline, the query plan should not be
// killed. Rather, we should be able to target the inner side to the new owning shard.
const innerCollName = db[kUnsplittable2CollName].getFullName();
cursor = coll.aggregate(pipeline, {batchSize: 1});
assert(cursor.hasNext());
assert.commandWorked(db.adminCommand({moveCollection: innerCollName, toShard: shard0}));
assert.doesNotThrow(() => iterateCursor(cursor));

st.stop();
