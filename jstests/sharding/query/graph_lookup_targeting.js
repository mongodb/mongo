/*
 * Test that the targeting of $graphLookup queries and any sub-queries works correctly.
 * @tags: [
 *   assumes_balancer_off,
 *   requires_sharding,
 *   requires_spawning_own_processes,
 *   requires_profiling,
 *   # Needed to run createUnsplittableCollection
 *   featureFlagAuthoritativeShardCollection,
 * ]
 */

import {ShardTargetingTest} from "jstests/libs/shard_targeting_util.js";

const kDbName = "graph_lookup_targeting";
const st = new ShardingTest({shards: 3});
const db = st.s.getDB(kDbName);
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard2 = st.shard2.shardName;
const shard0DB = st.shard0.getDB(kDbName);
const shard1DB = st.shard1.getDB(kDbName);
const shard2DB = st.shard2.getDB(kDbName);

assert.commandWorked(db.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

// Map from shard name to database connection. Used when determine which shard to read from when
// gathering profiler output.
const shardProfileDBMap = {
    [shard0]: shard0DB,
    [shard1]: shard1DB,
    [shard2]: shard2DB
};

const shardTargetingTest = new ShardTargetingTest(db, shardProfileDBMap);

// Create two sharded collections, both of which are distributed across the 3 shards.
const kShardedColl1Name = "sharded1";
const kShardedColl1Docs = [
    {_id: -2, a: -1},
    {_id: -1, a: 0},
    {_id: 0, a: 1},
    {_id: 1, a: 2},
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
    {_id: -2, a: 0},
    {_id: -1, a: 1},
    {_id: 0, a: -10},
    {_id: 1, a: 2},
    {_id: 2, a: -2},
];
const kShardedColl2ChunkList = [
    {min: {a: MinKey}, max: {a: 0}, shard: shard0},
    {min: {a: 0}, max: {a: 100}, shard: shard1},
    {min: {a: 100}, max: {a: MaxKey}, shard: shard2}
];
shardTargetingTest.setupColl({
    collName: kShardedColl2Name,
    indexList: [{a: 1}],
    docs: kShardedColl2Docs,
    collType: "sharded",
    shardKey: {a: 1},
    chunkList: kShardedColl2ChunkList
});

// Create two unsplittable collections.
const kUnsplittable1CollName = "unsplittable_1";
const kUnsplittable1Docs = [
    {_id: -1, a: 0, unsplittable: 1},
    {_id: 0, a: 1, unsplittable: 1},
    {_id: 1, a: 2, unsplittable: 1}
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
    {_id: -1, a: 1, unsplittable: 2},
    {_id: 1, a: 101, unsplittable: 2},
];
shardTargetingTest.setupColl({
    collName: kUnsplittable2CollName,
    docs: kUnsplittable2Docs,
    collType: "unsplittable",
    owningShard: shard2,
});

// Inner collection is unsplittable and not on the primary shard. Outer collection is sharded.
// In this case, we should execute in parallel on the shards.
let pipeline = [{$graphLookup: {from: kUnsplittable1CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}}];
let expectedResults = [
    {
        _id: -2,
        a: -1,
        links: [
            {_id: -1, a: 0, unsplittable: 1},
            {_id: 0, a: 1, unsplittable: 1},
            {_id: 1, a: 2, unsplittable: 1}
        ]
    },
    {_id: -1, a: 0, links: [{_id: 0, a: 1, unsplittable: 1}, {_id: 1, a: 2, unsplittable: 1}]},
    {_id: 0, a: 1, links: [{_id: 1, a: 2, unsplittable: 1}]},
    {_id: 1, a: 2, links: []},
    {_id: 2, a: 101, links: []},
];

let profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$graphLookup"]}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup"]},
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]},
    ],
    [shard2]: [{ns: kShardedColl1Name, expectedStages: ["$graphLookup"]}],
};

shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {expectedShardStages: ["$graphLookup"]},
    expectedResults: expectedResults,
    comment: "graph_lookup_outer_sharded_inner_unsplittable",
    profileFilters: profileFilters,
});

// Outer collection is unsplittable and not on the primary shard. Inner collection is sharded.
// We should target the shard which owns the unsplittable collection.
pipeline = [{$graphLookup: {from: kShardedColl1Name, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}}];
expectedResults = [
    {
        _id: -1,
        a: 0,
        unsplittable: 1,
        links: [
            {_id: 0, a: 1},
            {_id: 1, a: 2},
            {_id: 2, a: 101},
        ]
    },
    {
        _id: 0,
        a: 1,
        unsplittable: 1,
        links: [
            {_id: 1, a: 2},
            {_id: 2, a: 101},
        ]
    },
    {_id: 1, a: 2, unsplittable: 1, links: [{_id: 2, a: 101}]}
];

profileFilters = {
    [shard0]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: ["$match"]},
        {ns: kUnsplittable1CollName, expectedStages: ["$graphLookup"]}
    ],
    [shard2]: [{ns: kShardedColl1Name, expectedStages: ["$match"]}],
};

shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {expectedShard: shard1, expectedShardStages: ["$cursor", "$graphLookup"]},
    expectedResults: expectedResults,
    comment: "graph_lookup_outer_unsplittable_inner_sharded",
    profileFilters: profileFilters,
});

// Both collections are unsplittable and are located on different shards. We should execute the
// $graphLookup on the shard which owns the outer collection.
pipeline = [{$graphLookup: {from: kUnsplittable2CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}}];
expectedResults = [
    {
        _id: -1,
        a: 0,
        unsplittable: 1,
        links: [
            {_id: 0, a: -1, unsplittable: 2},
            {_id: -1, a: 1, unsplittable: 2},
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: 0,
        a: 1,
        unsplittable: 1,
        links: [
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {_id: 1, a: 2, unsplittable: 1, links: []},
];

profileFilters = {
    [shard0]: [],
    [shard1]: [
        {ns: kUnsplittable1CollName, expectedStages: []},
    ],
    [shard2]: [{ns: kUnsplittable1CollName, expectedStages: ["$mergeCursors", "$graphLookup"]}]
};

shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kUnsplittable1CollName,
    explainAssertionObj: {
        expectedMergingShard: shard2,
        expectedMergingStages: ["$mergeCursors", "$graphLookup"],
        expectedShard: shard1,
    },
    expectedResults: expectedResults,
    comment: "graph_lookup_outer_unsplittable_1_inner_unsplittable_2",
    profileFilters: profileFilters,
});

pipeline = [{$graphLookup: {from: kUnsplittable1CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}}];
expectedResults = [
    {
        _id: 0,
        a: -1,
        unsplittable: 2,
        links: [
            {_id: -1, a: 0, unsplittable: 1},
            {_id: 0, a: 1, unsplittable: 1},
            {_id: 1, a: 2, unsplittable: 1}
        ]
    },
    {_id: -1, a: 1, unsplittable: 2, links: [{_id: 1, a: 2, unsplittable: 1}]},
    {_id: 1, a: 101, unsplittable: 2, links: []},
];
profileFilters = {
    [shard0]: [],
    [shard1]: [{ns: kUnsplittable2CollName, expectedStages: ["$mergeCursors", "$graphLookup"]}],
    [shard2]: [{ns: kUnsplittable2CollName, expectedStages: []}],
};
shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kUnsplittable2CollName,
    explainAssertionObj: {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", "$graphLookup"],
        expectedShard: shard2,
    },
    expectedResults: expectedResults,
    comment: "graph_lookup_outer_unsplittable_2_inner_unsplittable_1",
    profileFilters: profileFilters,
});

// TODO SERVER-83902: Add test coverage where the inner collection targets a view whose pipeline
// includes a $graphLookup. In particular, we wish to have the following cases:
// - Nested case where we have [$graphLookup(A, [$graphLoookup(B)])]. A and B are unsplittable and
// on different shards.
// - Nested case where we have [$graphLookup(A, [$graphLookup(B)])]. A sharded, B unsplittable and
// not on the primary shard.
// - Nested case where we have [$graphLookup(A, [$graphLookup(B)])]. A unsplittable and not on
// the primary shard, B sharded.
// - Consider adding a case with 3 levels of nested pipelines

// Issue an aggregate targeting a sharded collection featuring two $graphLookup stages, where both
// stages' inner collections are unsplittable and reside on different shards We should execute the
// $graphLookups in parallel on the shards.
pipeline = [
    {$graphLookup: {from: kUnsplittable1CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}},
    {$graphLookup: {from: kUnsplittable2CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links_2"}}
];
expectedResults = [
    {
        _id: -2,
        a: -1,
        links: [
            {_id: -1, a: 0, unsplittable: 1},
            {_id: 0, a: 1, unsplittable: 1},
            {_id: 1, a: 2, unsplittable: 1}
        ],
        links_2: [
            {_id: -1, a: 1, unsplittable: 2},
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: -1,
        a: 0,
        links: [{_id: 0, a: 1, unsplittable: 1}, {_id: 1, a: 2, unsplittable: 1}],
        links_2: [
            {_id: 0, a: -1, unsplittable: 2},
            {_id: -1, a: 1, unsplittable: 2},
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: 0,
        a: 1,
        links: [{_id: 1, a: 2, unsplittable: 1}],
        links_2: [
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {_id: 1, a: 2, links: [], links_2: []},
    {_id: 2, a: 101, links: [], links_2: []},
];
profileFilters = {
    [shard0]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
    ],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]},
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kUnsplittable2CollName, expectedStages: ["$match"]},
    ],
};

shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedShardStages: ["$graphLookup", "$graphLookup"],
        expectedMergingStages: ["$mergeCursors"],
        expectMongos: true,
    },
    expectedResults: expectedResults,
    comment: "first_graph_lookup_inner_unsplittable_1_second_graph_lookup_inner_unsplittable_2",
    profileFilters: profileFilters,
});

pipeline = [
    {$graphLookup: {from: kUnsplittable2CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links_2"}},
    {$graphLookup: {from: kUnsplittable1CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}}
];
shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedShardStages: ["$graphLookup", "$graphLookup"],
        expectedMergingStages: ["$mergeCursors"],
        expectMongos: true,
    },
    expectedResults: expectedResults,
    comment: "first_graph_lookup_inner_unsplittable_2_second_graph_lookup_inner_unsplittable_1",
    profileFilters: profileFilters,
});

// Issue aggregates featuring two $graphLookup stages: the first $graphLookup targets an inner
// sharded collection and the other targets an unsplittable inner collection. We should execute both
// $graphLookup stages in parallel on the shards.
pipeline = [
    {$graphLookup: {from: kShardedColl2Name, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}},
    {$graphLookup: {from: kUnsplittable1CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links_2"}}
];
expectedResults = [
    {
        _id: -2,
        a: -1,
        links: [
            {_id: -2, a: 0},
            {_id: -1, a: 1},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [
            {_id: -1, a: 0, unsplittable: 1},
            {_id: 0, a: 1, unsplittable: 1},
            {_id: 1, a: 2, unsplittable: 1}
        ]
    },
    {
        _id: -1,
        a: 0,
        links: [
            {_id: 0, a: -10},
        ],
        links_2: [{_id: 0, a: 1, unsplittable: 1}, {_id: 1, a: 2, unsplittable: 1}]
    },
    {
        _id: 0,
        a: 1,
        links: [
            {_id: -2, a: 0},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [{_id: 1, a: 2, unsplittable: 1}]
    },
    {
        _id: 1,
        a: 2,
        links: [
            {_id: 2, a: -2},
            {_id: -2, a: 0},
            {_id: 0, a: -10},
        ],
        links_2: []
    },
    {_id: 2, a: 101, links: [], links_2: []},
];

profileFilters = {
    [shard0]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]}
    ],
    [shard1]: [
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]},
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]}
    ],
};
shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectedShardStages: ["$graphLookup", "$graphLookup"],
        expectedMergingStages: ["$mergeCursors"],
        expectMongos: true,
    },
    expectedResults: expectedResults,
    comment: "first_graph_lookup_inner_sharded_second_graph_lookup_inner_unsplittable_1",
    profileFilters: profileFilters,
});

pipeline =  [
    {$graphLookup: {from: kShardedColl2Name, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}},
    {$graphLookup: {from: kUnsplittable2CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links_2"}}
];
expectedResults = [
    {
        _id: -2,
        a: -1,
        links: [
            {_id: -2, a: 0},
            {_id: -1, a: 1},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [
            {_id: -1, a: 1, unsplittable: 2},
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: -1,
        a: 0,
        links: [
            {_id: 0, a: -10},
        ],
        links_2: [
            {_id: 0, a: -1, unsplittable: 2},
            {_id: -1, a: 1, unsplittable: 2},
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: 0,
        a: 1,
        links: [
            {_id: -2, a: 0},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: 1,
        a: 2,
        links: [
            {_id: -2, a: 0},
            {_id: 0, a: -10},
            {_id: 2, a: -2},
        ],
        links_2: []
    },
    {_id: 2, a: 101, links: [], links_2: []},
];
profileFilters = {
    [shard0]: [
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]}
    ],
    [shard1]: [
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]}
    ],
    [shard2]: [
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kUnsplittable2CollName, expectedStages: ["$match"]},
    ]
};

shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectMongos: true,
        expectedShardStages: ["$graphLookup", "$graphLookup"],
        expectedMergingStages: ["$mergeCursors"],
    },
    expectedResults: expectedResults,
    comment: "first_graph_lookup_inner_sharded_second_graph_lookup_inner_unsplittable_2",
    profileFilters: profileFilters,
});

// Issue aggregates targeting a sharded collection featuring two $graphLookup stages, where the
// first one's inner collection is unsplittable and the second one's inner collection is sharded. We
// should execute both $graphLookup stages in parallel on the shards.
pipeline = [
    {$graphLookup: {from: kUnsplittable1CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links_2"}},
    {$graphLookup: {from: kShardedColl2Name, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}}
];
expectedResults = [
    {
        _id: -2,
        a: -1,
        links: [
            {_id: -2, a: 0},
            {_id: -1, a: 1},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [
            {_id: -1, a: 0, unsplittable: 1},
            {_id: 0, a: 1, unsplittable: 1},
            {_id: 1, a: 2, unsplittable: 1}
        ]
    },
    {
        _id: -1,
        a: 0,
        links: [
            {_id: 0, a: -10},
        ],
        links_2: [{_id: 0, a: 1, unsplittable: 1}, {_id: 1, a: 2, unsplittable: 1}]
    },
    {
        _id: 0,
        a: 1,
        links: [
            {_id: -2, a: 0},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [{_id: 1, a: 2, unsplittable: 1}]
    },
    {
        _id: 1,
        a: 2,
        links: [
            {_id: 2, a: -2},
            {_id: -2, a: 0},
            {_id: 0, a: -10},
        ],
        links_2: []
    },
    {_id: 2, a: 101, links: [], links_2: []},
];
profileFilters = {
    [shard0]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
    ],
    [shard1]: [
        {ns: kUnsplittable1CollName, expectedStages: ["$match"]},
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
    ],
    [shard2]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
    ],
};

shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectMongos: true,
        expectedShardStages: ["$graphLookup", "$graphLookup"],
        expectedMergingStages: ["$mergeCursors"],
    },
    expectedResults: expectedResults,
    comment: "first_graph_lookup_inner_unsplittable_1_second_graph_lookup_inner_sharded",
    profileFilters: profileFilters,
});

pipeline = [
    {$graphLookup: {from: kUnsplittable2CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links_2"}},
    {$graphLookup: {from: kShardedColl2Name, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}}
];
expectedResults = [
    {
        _id: -2,
        a: -1,
        links: [
            {_id: -2, a: 0},
            {_id: -1, a: 1},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [
            {_id: -1, a: 1, unsplittable: 2},
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: -1,
        a: 0,
        links: [
            {_id: 0, a: -10},
        ],
        links_2: [
            {_id: 0, a: -1, unsplittable: 2},
            {_id: -1, a: 1, unsplittable: 2},
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: 0,
        a: 1,
        links: [
            {_id: -2, a: 0},
            {_id: 0, a: -10},
            {_id: 1, a: 2},
            {_id: 2, a: -2},
        ],
        links_2: [
            {_id: 1, a: 101, unsplittable: 2},
        ]
    },
    {
        _id: 1,
        a: 2,
        links: [
            {_id: -2, a: 0},
            {_id: 0, a: -10},
            {_id: 2, a: -2},
        ],
        links_2: []
    },
    {_id: 2, a: 101, links: [], links_2: []},
];
profileFilters = {
    [shard0]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
    ],
    [shard1]: [
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
    ],
    [shard2]: [
        {ns: kUnsplittable2CollName, expectedStages: ["$match"]},
        {ns: kShardedColl1Name, expectedStages: ["$graphLookup", "$graphLookup"]},
        {ns: kShardedColl2Name, expectedStages: ["$match"]},
    ],
};
shardTargetingTest.assertShardTargeting({
    pipeline: pipeline,
    targetCollName: kShardedColl1Name,
    explainAssertionObj: {
        expectMongos: true,
        expectedShardStages: ["$graphLookup", "$graphLookup"],
        expectedMergingStages: ["$mergeCursors"],
    },
    expectedResults: expectedResults,
    comment: "first_graph_lookup_inner_unsplittable_2_second_graph_lookup_inner_sharded",
    profileFilters: profileFilters,
});

// Set of tests which involve moving an unsplittable collection during query execution.

// Test moving the outer collection to another shard during $group execution. This should
// result in a 'QueryPlanKilled' error.
let coll = db[kUnsplittable1CollName];

// Add a set of 20 documents to the outer collection which are at least 1 MB in size. This
// makes it so that the documents from the outer collection do not fit in one batch and we
// will have to issue at least one getMore against the outer collection to continue
// constructing the result set.
const startingId = 2;
let docsToAdd = [];
const bigStr = "a".repeat(1024 * 1024);
for (let i = startingId; i < startingId + 20; i++) {
    docsToAdd.push({_id: i, a: i + 1, str: bigStr});
}
assert.commandWorked(coll.insertMany(docsToAdd));

// Establish our cursor. We should not have exhausted our cursor.
pipeline = [
    {$graphLookup: {from: kUnsplittable2CollName, startWith: "$a", connectFromField: "a", connectToField: "_id", as: "links"}},
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

// Test moving the inner collection to another shard during $graphLookup execution. Because the
// move happens in between executions of the inner pipeline, the query plan should not be
// killed. Rather, we should be able to target the inner side to the new owning shard.
const innerCollName = db[kUnsplittable2CollName].getFullName();
cursor = coll.aggregate(pipeline, {batchSize: 1});
assert(cursor.hasNext());
assert.commandWorked(db.adminCommand({moveCollection: innerCollName, toShard: shard0}));
assert.doesNotThrow(() => iterateCursor(cursor));

st.stop();
