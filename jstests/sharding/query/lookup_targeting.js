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

import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {ShardTargetingTest} from "jstests/libs/shard_targeting_util.js";

const kDbName = "lookup_targeting";
const st = new ShardingTest({shards: 3, mongos: 2});
const db = st.s.getDB(kDbName);
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard2 = st.shard2.shardName;
const shard0DB = st.shard0.getDB(kDbName);
const shard1DB = st.shard1.getDB(kDbName);
const shard2DB = st.shard2.getDB(kDbName);

assert.commandWorked(db.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

// Map from shard name to database connection. Used to determine which shard to read from when
// gathering profiler output.
const shardDBMap = {
    [shard0]: shard0DB,
    [shard1]: shard1DB,
    [shard2]: shard2DB
};

const shardTargetingTest = new ShardTargetingTest(db, shardDBMap);

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
shardTargetingTest.setupColl({
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
shardTargetingTest.setupColl({
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
shardTargetingTest.setupColl({
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
shardTargetingTest.setupColl({
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
shardTargetingTest.setupColl({
    collName: kUnsplittable3CollName,
    docs: kUnsplittable3Docs,
    collType: "unsplittable",
    owningShard: shard2,
});

// Inner collection is unsplittable and not on the primary shard. Outer collection is sharded.
// In this case, we should be merging on the shard which owns the unsplittable collection.
let expectedResults = [
    {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 1}]},
    {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1}]},
    {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 1}]},
];
let profileFilters = {
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
};

shardTargetingTest.assertShardTargeting({
    pipeline:
        [{$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kShardedColl1Name,
    explainAssertionObj:
        {expectedMergingShard: shard1, expectedMergingStages: ["$mergeCursors", "$lookup"]},
    expectedResults: expectedResults,
    comment: "outer_sharded_inner_unsplittable",
    profileFilters: profileFilters
});

expectedResults = [
    {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1}]},
    {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1}]},
    {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101}]},
];
profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
    [shard1]: [{ns: kUnsplittable1CollName, expectedStages: ["$lookup"]}],
    [shard2]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
};

// Outer collection is unsplittable and not on the primary shard. Inner collection is sharded.
// We should target the shard which owns the unsplittable collection.
shardTargetingTest.assertShardTargeting({
    pipeline: [{$lookup: {from: kShardedColl1Name, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {expectedShard: shard1, expectedShardStages: ["$cursor", "$lookup"]},
    expectedResults: expectedResults,
    comment: "outer_unsplittable_inner_sharded",
    profileFilters: profileFilters,
});

expectedResults = [
    {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]},
    {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]},
    {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]},
];
profileFilters = {
    [shard1]: [{ns: kUnsplittable1CollName, expectedStages: []}],
    [shard2]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$lookup"]}]
};

// Both collections are unsplittable and are located on different shards. We should merge on the
// shard which owns the inner collection in each case.
shardTargetingTest.assertShardTargeting({
    pipeline:
        [{$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
        expectedShard: shard1,
    },
    expectedResults: expectedResults,
    comment: "outer_unsplittable_1_inner_unsplittable_2",
    profileFilters: profileFilters,
});

expectedResults = [
    {_id: 0, a: -1, unsplittable: 2, out: [{_id: 0, a: -1, unsplittable: 1}]},
    {_id: 1, a: 1, unsplittable: 2, out: [{_id: 1, a: 1, unsplittable: 1}]},
    {_id: 2, a: 101, unsplittable: 2, out: [{_id: 2, a: 101, unsplittable: 1}]},
];
profileFilters = {
    [shard1]: [{ns: kUnsplittable2CollName, expectedStages: ["$mergeCursors", "$lookup"]}],
    [shard2]: [{ns: kUnsplittable2CollName, expectedStages: []}],
};
shardTargetingTest.assertShardTargeting({
    pipeline:
        [{$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable2CollName,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
        expectedShard: shard2,
    },
    expectedResults: expectedResults,
    comment: "outer_unsplittable_2_inner_unsplittable_1",
    profileFilters: profileFilters,
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

expectedResults = [
    {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]},
    {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]},
    {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]},
];
profileFilters = {
    [shard0]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$lookup"]}],
    [shard1]: [{ns: kUnsplittable1CollName, expectedStages: []}],
    [shard2]: [{ns: kUnsplittable2CollName, expectedStages: ["$match"]}],
};
shardTargetingTest.assertShardTargeting({
    pipeline: [{$lookup: {from: kViewName, localField: "a", foreignField: "a", as: "out"}}],
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {
        expectedMergingShard: shard0,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
        expectedShard: shard1,
    },
    expectedResults: expectedResults,
    comment: "lookup_inner_side_targets_view_over_unsplittable_2",
    profileFilters: profileFilters,
});

// Clean up the view.
assert(db[kViewName].drop());

if (checkSbeRestrictedOrFullyEnabled(db)) {
    // Both collections are unsplittable and are collocated on the same shard. Test that we can do
    // SBE $lookup pushdown, regardless of which collection is on the inner side.
    expectedResults = [
        {_id: 0, a: -1, unsplittable: 3, out: [{_id: 0, a: -1, unsplittable: 2}]},
        {_id: 1, a: 1, unsplittable: 3, out: [{_id: 1, a: 1, unsplittable: 2}]},
        {_id: 2, a: 101, unsplittable: 3, out: [{_id: 2, a: 101, unsplittable: 2}]},
    ];
    shardTargetingTest.assertShardTargeting({
        pipeline: [
            {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}}
        ],
        targetCollName: kUnsplittable3CollName,
        explainAssertionObj: {
            expectedShard: shard2,
            assertSBELookupPushdown: true,
        },
        expectedResults: expectedResults,
        comment: "lookup_sbe_pushdown_target_unsplittable_3",
    });

    expectedResults = [
        {_id: 0, a: -1, unsplittable: 2, out: [{_id: 0, a: -1, unsplittable: 3}]},
        {_id: 1, a: 1, unsplittable: 2, out: [{_id: 1, a: 1, unsplittable: 3}]},
        {_id: 2, a: 101, unsplittable: 2, out: [{_id: 2, a: 101, unsplittable: 3}]},
    ];
    shardTargetingTest.assertShardTargeting({
        pipeline: [
            {$lookup: {from: kUnsplittable3CollName, localField: "a", foreignField: "a", as: "out"}}
        ],
        targetCollName: kUnsplittable2CollName,
        explainAssertionObj: {
            expectedShard: shard2,
            assertSBELookupPushdown: true,
        },
        expectedResults: expectedResults,
        comment: "lookup_sbe_pushdown_target_unsplittable_2",
    });
}

// Verify the targeting behavior of $facet. In particular, we should always merge or target the
// shard corresponding to the first inner unsplittable collection among the $lookup facet pipelines.
expectedResults = [
    {
        pipe1: [
            {_id: 0, a: -1, out_1: [{_id: 0, a: -1, unsplittable: 1}]},
            {_id: 1, a: 1, out_1: [{_id: 1, a: 1, unsplittable: 1}]},
            {_id: 2, a: 101, out_1: [{_id: 2, a: 101, unsplittable: 1}]},

        ],
        pipe2: [
            {_id: 0, a: -1, out_2: [{_id: 0, a: -1, unsplittable: 2}]},
            {_id: 1, a: 1, out_2: [{_id: 1, a: 1, unsplittable: 2}]},
            {_id: 2, a: 101, out_2: [{_id: 2, a: 101, unsplittable: 2}]},
        ]
    },
];
profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$facet"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable2CollName, expectedStages: ["$match"]}
    ],
};

shardTargetingTest.assertShardTargeting({
    pipeline: [{
        $facet: {
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
    explainAssertionObj:
        {expectedMergingShard: shard1, expectedMergingStages: ["$mergeCursors", "$facet"]},
    expectedResults: expectedResults,
    comment: "facet_lookup_pipeline_merge_on_shard_1",
    profileFilters: profileFilters,
});

expectedResults = [
    {
        pipe1: [
            {_id: 0, a: -1, out_2: [{_id: 0, a: -1, unsplittable: 2}]},
            {_id: 1, a: 1, out_2: [{_id: 1, a: 1, unsplittable: 2}]},
            {_id: 2, a: 101, out_2: [{_id: 2, a: 101, unsplittable: 2}]},
        ],
        pipe2: [
            {_id: 0, a: -1, out_1: [{_id: 0, a: -1, unsplittable: 1}]},
            {_id: 1, a: 1, out_1: [{_id: 1, a: 1, unsplittable: 1}]},
            {_id: 2, a: 101, out_1: [{_id: 2, a: 101, unsplittable: 1}]},
        ],
    },
];
profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$facet"]}
    ],
};

shardTargetingTest.assertShardTargeting({
    pipeline: [{
        $facet: {
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
    explainAssertionObj:
        {expectedMergingShard: shard2, expectedMergingStages: ["$mergeCursors", "$facet"]},
    expectedResults: expectedResults,
    comment: "facet_lookup_pipeline_merge_on_shard_2",
    profileFilters: profileFilters,
});

// If our initial query targets a single shard and that shard is the same as the merging shard, then
// we can target the facet to a single shard (even if we need to issue remotes reads for the inner
// side of the first $lookup).
profileFilters = {
    [shard0]: [],
    [shard1]: [{ns: kShardedColl1Name, expectedStages: ["$match", "$facet"]}],
    [shard2]: [{ns: kUnsplittable2CollName, expectedStages: ["$match"]}],
};
expectedResults = [
    {
        pipe1: [
            {_id: 1, a: 1, out_1: [{_id: 1, a: 1, unsplittable: 1}]},
        ],
        pipe2: [
            {_id: 1, a: 1, out_2: [{_id: 1, a: 1, unsplittable: 2}]},
        ],
    },
];
shardTargetingTest.assertShardTargeting({
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
    expectedResults: expectedResults,
    comment: "facet_lookup_pipeline_target_single_shard",
    profileFilters: profileFilters,
});

expectedResults = [
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
];
profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup", "$lookup"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable2CollName, expectedStages: ["$match"]}
    ],
};

// Issue an aggregate featuring two $lookup stages, where both stages' inner collections are
// unsplittable and reside on different shards. We should always merge on the shard which owns the
// first inner collection.
shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"],
    },
    expectedResults: expectedResults,
    comment: "first_lookup_inner_unsplittable_1_second_lookup_inner_unsplittable_2",
    profileFilters: profileFilters,
});

expectedResults = [
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
];
profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup", "$lookup"]}
    ]
};

shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"],
    },
    expectedResults: expectedResults,
    comment: "first_lookup_inner_unsplittable_2_second_lookup_inner_unsplittable_1",
    profileFilters: profileFilters,
});

// Issue aggregates featuring two $lookup stages: one $lookup targets an unsplittable inner
// collection and the other targets a sharded inner collection. We should merge on the shard which
// owns the unsplittable collection.
expectedResults = [
    {_id: 0, a: -1, out: [{_id: 0, b: -1}], out_2: [{_id: 0, a: -1, unsplittable: 1}]},
    {_id: 1, a: 1, out: [{_id: 1, b: 1}], out_2: [{_id: 1, a: 1, unsplittable: 1}]},
    {_id: 2, a: 101, out: [{_id: 2, b: 101}], out_2: [{_id: 2, a: 101, unsplittable: 1}]},
];
profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$lookup"]}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}
    ],
    [shard2]: [{ns: kShardedColl1Name, expectedStages: ["$lookup"]}],
},
    shardTargetingTest.assertShardTargeting({
        pipeline: [
            {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out"}},
            {
                $lookup:
                    {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out_2"}
            },
        ],
        targetCollName: kShardedColl1Name,
        explainAssertionObj: {
            expectedMergingShard: shard1,
            expectedMergingStages: ["$mergeCursors", "$lookup"],
        },
        expectedResults: expectedResults,
        comment: "first_lookup_inner_sharded_second_lookup_inner_unsplittable_1",
        profileFilters: profileFilters,
    });

expectedResults = [
    {_id: 0, a: -1, out: [{_id: 0, b: -1}], out_2: [{_id: 0, a: -1, unsplittable: 2}]},
    {_id: 1, a: 1, out: [{_id: 1, b: 1}], out_2: [{_id: 1, a: 1, unsplittable: 2}]},
    {_id: 2, a: 101, out: [{_id: 2, b: 101}], out_2: [{_id: 2, a: 101, unsplittable: 2}]},
];
profileFilters = {
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
};
shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out"}},
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
    },
    expectedResults: expectedResults,
    comment: "first_lookup_inner_sharded_second_lookup_inner_unsplittable_2",
    profileFilters: profileFilters,
});

// Issue aggregates featuring two $lookup stages, where the first one's inner collection is
// unsplittable and the second one's inner collection is sharded. We should target the shard which
// owns the unsplittable collection.
expectedResults = [
    {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 1}], out_2: [{_id: 0, b: -1}]},
    {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1}], out_2: [{_id: 1, b: 1}]},
    {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 1}], out_2: [{_id: 2, b: 101}]},
];
profileFilters = {
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
};

shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"]
    },
    expectedResults: expectedResults,
    comment: "first_lookup_inner_unsplittable_1_second_lookup_inner_sharded",
    profileFilters: profileFilters,
});

expectedResults = [
    {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 2}], out_2: [{_id: 0, b: -1}]},
    {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 2}], out_2: [{_id: 1, b: 1}]},
    {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 2}], out_2: [{_id: 2, b: 101}]},
];
profileFilters = {
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
};
shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}},
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out_2"}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup", "$lookup"]
    },
    expectedResults: expectedResults,
    comment: "first_lookup_inner_unsplittable_2_second_lookup_inner_sharded",
    profileFilters: profileFilters,
});

// Issue an aggregate featuring nested $lookup stages where both inner collections are unsplittable
// and live on different shards. The $lookup should execute on the shard which owns of the first
// inner collection. Then, we should have several $match queries against each of the inner
// collections.
expectedResults = [
    {
        _id: 0,
        a: -1,
        out: [{_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]}]
    },
    {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]}]},
    {
        _id: 2,
        a: 101,
        out: [{_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]}]
    },
];
profileFilters = {
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
};
shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
            {$lookup: {from: kUnsplittable2CollName , localField: "a", foreignField: "a", as: "out"}},
        ]}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$lookup"],
    },
    expectedResults: expectedResults,
    comment: "nested_lookup_inner_unsplittable_1_innermost_unsplittable_2",
    profileFilters: profileFilters,
});

expectedResults = [
    {
        _id: 0,
        a: -1,
        out: [{_id: 0, a: -1, unsplittable: 2, out: [{_id: 0, a: -1, unsplittable: 1}]}]
    },
    {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 2, out: [{_id: 1, a: 1, unsplittable: 1}]}]},
    {
        _id: 2,
        a: 101,
        out: [{_id: 2, a: 101, unsplittable: 2, out: [{_id: 2, a: 101, unsplittable: 1}]}]
    },
];
profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: []}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]},
        {ns: kUnsplittable2CollName, expectedStages: ["$match"]}
    ],
};
shardTargetingTest.assertShardTargeting({
    pipeline:
        [
            {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
                {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}},
            ]}},
    ],
    targetCollName: kShardedColl1Name,
    explainAssertionObj: { expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$lookup"]},
    expectedResults: expectedResults,
    comment: "nested_lookup_inner_unsplittable_2_innermost_unsplittable_1",
    profileFilters: profileFilters,
});

// Issue an aggregate featuring nested $lookup stages where the innermost collection is sharded
// and the top level $lookup's 'from' collection is unsplittable. We should execute on the shard
// which owns the unsplittable collection.
expectedResults = [
    {_id: 0, a: -1, out: [{_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, b: -1}]}]},
    {_id: 1, a: 1, out: [{_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, b: 1}]}]},
    {_id: 2, a: 101, out: [{_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, b: 101}]}]},
];
profileFilters = {
    [shard0]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl2Name, expectedStages: ["$match"]}
    ],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kShardedColl2Name, expectedStages: ["$match"]}
    ]
};
shardTargetingTest.assertShardTargeting({
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
    expectedResults: expectedResults,
    comment: "nested_lookup_inner_unsplittable_1_innermost_sharded_2",
    profileFilters: profileFilters,
});

// Issue an aggregate featuring nested $lookup stages where the innermost collection is
// unsplittable, while the top level $lookup's 'from' collection is sharded.
expectedResults = [
    {_id: 0, a: -1, out: [{_id: 0, b: -1, out: [{_id: 0, a: -1, unsplittable: 1}]}]},
    {_id: 1, a: 1, out: [{_id: 1, b: 1, out: [{_id: 1, a: 1, unsplittable: 1}]}]},
    {_id: 2, a: 101, out: [{_id: 2, b: 101, out: [{_id: 2, a: 101, unsplittable: 1}]}]},
];
profileFilters = {
    [shard0]: [
        {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]}
    ],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: ["$lookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]}
    ]
};
shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out", pipeline: [
            {$lookup: {from: kUnsplittable1CollName, localField: "b", foreignField: "a", as: "out"}},
        ]}}
    ],
    targetCollName: kShardedColl1Name,
    expectedResults: expectedResults,
    comment:  "nested_lookup_inner_sharded_innermost_unsplittable_1",
    profileFilters: profileFilters,
});

// Issue an aggregate featuring three levels of nested $lookups. All involved collections are
// unsplittable, and all live on different shards. However, we expect to merge on the shard which
// owns the top-level inner collection (in this case, shard1).
assert.commandWorked(
    db.adminCommand({moveCollection: db[kUnsplittable3CollName].getFullName(), toShard: shard0}));
expectedResults = [
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
];
profileFilters = {
    [shard0]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable3CollName, expectedStages: ["$match"]}
    ],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]},
        {ns: kShardedColl1Name, expectedStages: ["$mergeCursors", "$lookup"]}
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: []},
        {ns: kUnsplittable2CollName, expectedStages: ["$match"]}
    ]
};
shardTargetingTest.assertShardTargeting({
    pipeline: [
        {$lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
            {$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out", pipeline: [
                {$lookup: {from: kUnsplittable3CollName, localField: "a", foreignField: "a", as: "out"}},
        ]}}]}}
    ],
    targetCollName: kShardedColl1Name,
    expectedResults: expectedResults,
    comment:  "only_test_case_with_three_nested_pipelines",
    profileFilters: profileFilters,
});

// ----------------------------------------
// Tests with stale router
// Router believes outer and inner are not collocated, but they are.
{
    // Move kUnsplittable2CollName from shard2 to shard1. Use a mongos1 so that mongos0 is left
    // stale.
    assert.commandWorked(st.s1.adminCommand(
        {moveCollection: db[kUnsplittable2CollName].getFullName(), toShard: st.shard1.shardName}));

    let pipeline =
        [{$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}}];

    let expectedResults = [
        {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]},
        {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]},
        {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]},
    ];

    // Run the aggregation one first time to let the router gossip in the new placement for the
    // inner collection. Expect correct results, but sub-optimal pipeline splitting choice (pipeline
    // gets split in this run).
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedMergingShard: shard2,
            expectedMergingStages: ["$mergeCursors", "$lookup"],
        },
        expectedResults: expectedResults,
        comment: "outer_unsplittable_1_inner_unsplittable_2_collocated_but_stale_router_1",
    });

    // Check that router now routes optimally for the new placement.
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedShard: shard1,
            assertSBELookupPushdown: true,
        },
        expectedResults: expectedResults,
        comment: "outer_unsplittable_1_inner_unsplittable_2_collocated_but_stale_router_2",
        profileFilters: {
            [shard0]: [],
            [shard1]: [{ns: kUnsplittable1CollName, expectedStages: ["$lookup"]}],
            [shard2]: []
        }
    });
}

// Router believes outer and inner are collocated, but they are not anymore.
{
    // Return kUnsplittable2CollName to shard2. Use mongos1 so that mongos0 is left stale.
    assert.commandWorked(st.s1.adminCommand(
        {moveCollection: db[kUnsplittable2CollName].getFullName(), toShard: st.shard2.shardName}));

    let pipeline =
        [{$lookup: {from: kUnsplittable2CollName, localField: "a", foreignField: "a", as: "out"}}];

    let expectedResults = [
        {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1, unsplittable: 2}]},
        {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1, unsplittable: 2}]},
        {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101, unsplittable: 2}]},
    ];

    // Run the aggregation one first time to let the router gossip in the new placement for the
    // inner collection. Expect correct results, but sub-optimal merging shard choice (no pipeline
    // split in this case. The whole pipeline is sent to shard1).
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedShard: shard1,
        },
        expectedResults: expectedResults,
        comment: "outer_unsplittable_1_inner_unsplittable_2_not_collocated_but_stale_router_1",
    });

    // Check that router now routes optimally for the new placement.
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedMergingShard: shard2,
            expectedMergingStages: ["$mergeCursors", "$lookup"],
        },
        expectedResults: expectedResults,
        comment: "outer_unsplittable_1_inner_unsplittable_2_not_collocated_but_stale_router_2",
        profileFilters: {
            [shard0]: [],
            [shard1]: [{ns: kUnsplittable1CollName, expectedStages: []}],
            [shard2]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$lookup"]}],
        }
    });
}

// Router believes inner is sharded, but it is not anymore.
{
    // Unshard kShardedColl1Name and place it on shard2. Use mongos1 so that mongos0 is left stale.
    assert.commandWorked(st.s1.adminCommand(
        {unshardCollection: db[kShardedColl1Name].getFullName(), toShard: st.shard2.shardName}));

    let pipeline =
        [{$lookup: {from: kShardedColl1Name, localField: "a", foreignField: "a", as: "out"}}];

    let expectedResults = [
        {_id: 0, a: -1, unsplittable: 1, out: [{_id: 0, a: -1}]},
        {_id: 1, a: 1, unsplittable: 1, out: [{_id: 1, a: 1}]},
        {_id: 2, a: 101, unsplittable: 1, out: [{_id: 2, a: 101}]},
    ];

    // Run the aggregation one first time to let the router gossip in the new placement for the
    // inner collection. Expect correct results, but sub-optimal merging shard choice (no pipeline
    // split in this case).
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedShard: shard1,
        },
        expectedResults: expectedResults,
        comment:
            "outer_unsplittable_1_inner_unsplittable_not_collocated_but_stale_router_believes_inner_is_sharded",
    });

    // Check that router now routes optimally for the new placement (uses shard2 as merging shard).
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedMergingShard: shard2,
            expectedMergingStages: ["$mergeCursors", "$lookup"],
        },
        expectedResults: expectedResults,
        comment:
            "outer_unsplittable_1_inner_unsplittable_not_collocated_but_stale_router_believes_inner_is_sharded",
        profileFilters: {
            [shard0]: [],
            [shard1]: [{ns: kUnsplittable1CollName, expectedStages: []}],
            [shard2]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$lookup"]}],
        }
    });

    // Reset kShardedColl1Name to leave it as it was.
    db[kShardedColl1Name].drop();
    shardTargetingTest.setupColl({
        collName: kShardedColl1Name,
        indexList: [{a: 1}],
        docs: kShardedColl1Docs,
        collType: "sharded",
        shardKey: {a: 1},
        chunkList: kShardedColl1ChunkList
    });
}

// Multiple secondary collections. Router believes they are all sharded, but one of them is not
// anymore.
{
    // Unshard kShardedColl2Name and place it on shard2. Use mongos1 so that mongos0 is left stale.
    assert.commandWorked(st.s1.adminCommand(
        {unshardCollection: db[kShardedColl2Name].getFullName(), toShard: st.shard2.shardName}));

    let pipeline = [{
        $facet: {
            pipe1: [{
                $lookup: {from: kShardedColl1Name, localField: "a", foreignField: "a", as: "out_1"}
            }],
            pipe2: [{
                $lookup: {from: kShardedColl2Name, localField: "a", foreignField: "b", as: "out_2"}
            }]
        }
    }];

    let expectedResults = [
        {
            pipe1: [
                {_id: 0, a: -1, unsplittable: 1, out_1: [{_id: 0, a: -1}]},
                {_id: 1, a: 1, unsplittable: 1, out_1: [{_id: 1, a: 1}]},
                {_id: 2, a: 101, unsplittable: 1, out_1: [{_id: 2, a: 101}]},

            ],
            pipe2: [
                {_id: 0, a: -1, unsplittable: 1, out_2: [{_id: 0, b: -1}]},
                {_id: 1, a: 1, unsplittable: 1, out_2: [{_id: 1, b: 1}]},
                {_id: 2, a: 101, unsplittable: 1, out_2: [{_id: 2, b: 101}]},
            ]
        },
    ];

    // Run the aggregation one first time to let the router gossip in the new placement for the
    // inner collections. Expect correct results, but sub-optimal merging shard choice (no pipeline
    // split in this case).
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedShard: shard1,
        },
        expectedResults: expectedResults,
        comment: "facet_inner_sharded_and_unsharded_but_router_stale_1",
    });

    // Check that router now routes optimally for the new placement (uses shard2 as merging
    // shard).
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUnsplittable1CollName,
        explainAssertionObj: {
            expectedMergingShard: shard2,
            expectedMergingStages: ["$mergeCursors", "$facet"],
        },
        expectedResults: expectedResults,
        comment: "facet_inner_sharded_and_unsharded_but_router_stale_2",
        profileFilters: {
            [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
            [shard1]: [
                {ns: kUnsplittable1CollName, expectedStages: []},
                {ns: kShardedColl1Name, expectedStages: ["$match"]}
            ],
            [shard2]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$facet"]}],
        }
    });

    // Reset kShardedColl2Name to leave it as it was.
    assert(db[kShardedColl2Name].drop());
    shardTargetingTest.setupColl({
        collName: kShardedColl2Name,
        indexList: [{b: 1}],
        docs: kShardedColl2Docs,
        collType: "sharded",
        shardKey: {b: 1},
        chunkList: kShardedColl2ChunkList
    });
}

// ----------------------------------------
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
