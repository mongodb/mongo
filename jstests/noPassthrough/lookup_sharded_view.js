// Test that sharded $lookup can resolve sharded views correctly.
// @tags: [requires_sharding, requires_fcv_51]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/profiler.js");             // For profilerHasSingleMatchingEntryOrThrow.

const sharded = new ShardingTest({
    mongos: 1,
    shards: [{verbose: 3}, {verbose: 3}, {verbose: 3}, {verbose: 3}],
    config: 1,
});
assert(sharded.adminCommand({enableSharding: "test"}));

const testDBName = "test";
const testDB = sharded.getDB(testDBName);
const primaryShard = sharded.shard0;
sharded.ensurePrimaryShard(testDBName, primaryShard.shardName);

// Drop the 'profile' tables and then enable profiling on all shards.
for (const shard of [sharded.shard0, sharded.shard1, sharded.shard2]) {
    const db = shard.getDB(testDBName);
    db.system.profile.drop();
    assert.commandWorked(db.setProfilingLevel(2));
}

// Create 'local' collection which will be backed by shard 1 from which we will run aggregations.
const local = testDB.local;
local.drop();
const localDocs = [
    {_id: 1, shard_key: "shard1", f: 1},
    {_id: 2, shard_key: "shard1", f: 2},
    {_id: 3, shard_key: "shard1", f: 3},
];
assert.commandWorked(local.createIndex({shard_key: 1}));
assert.commandWorked(local.insertMany(localDocs));
assert(sharded.s.adminCommand({shardCollection: local.getFullName(), key: {shard_key: 1}}));

// Create first 'foreign' collection which will be backed by shard 2.
const foreign = testDB.foreign;
foreign.drop();
const foreignDocs = [
    {join_field: 1, _id: 4, shard_key: "shard2", f: "a"},
    {join_field: 2, _id: 5, shard_key: "shard2", f: "b"},
    {join_field: 3, _id: 6, shard_key: "shard2", f: "c"},
];
assert.commandWorked(foreign.createIndex({shard_key: 1}));
assert.commandWorked(foreign.insertMany(foreignDocs));
assert(sharded.s.adminCommand({shardCollection: foreign.getFullName(), key: {shard_key: 1}}));

// Create second 'otherForeign' collection which will be backed by shard 3.
const otherForeign = testDB.otherForeign;
otherForeign.drop();
const otherForeignDocs = [
    {_id: 7, shard_key: "shard3"},
    {_id: 8, shard_key: "shard3"},
];
assert.commandWorked(otherForeign.createIndex({shard_key: 1}));
assert.commandWorked(otherForeign.insertMany(otherForeignDocs));
assert(sharded.s.adminCommand({shardCollection: otherForeign.getFullName(), key: {shard_key: 1}}));

let testCount = 0;

// To resolve view 'view1' defined on view 'view2' defined on collection 'coll', we would get a
// dependency chain of the form [view1, view2, coll] in the 'resolvedViews' field of the
// system.profile entry. This function combines all namespaces listed in the dependency chains of an
// aggregation's resolved views to produce a map counting how many times each namesapce was seen,
// e.g.: {"test.view1": 1, "test.view2": 1, "test.coll": 1}
function getShardedViewExceptions(comment) {
    return primaryShard.getDB(testDBName)
        .system.profile.find({})
        .toArray()
        .filter((doc) => doc["command"] && doc["command"]["aggregate"] && doc["resolvedViews"] &&
                    doc["command"]["comment"] === comment)
        .flatMap((doc) =>
                     doc["resolvedViews"].flatMap(resolvedView => resolvedView["dependencyChain"]))
        .reduce((prev, curr) => {
            const namespace = "test." + curr;
            if (prev[namespace]) {
                prev[namespace] += 1;
            } else {
                prev[namespace] = 1;
            }
            return prev;
        }, {});
}

function testLookupView({pipeline, expectedResults, expectedExceptions}) {
    const comment = "test " + testCount;
    assertArrayEq(
        {actual: local.aggregate(pipeline, {comment}).toArray(), expected: expectedResults});
    if (expectedExceptions) {
        // Count how many CommandOnShardedViewNotSupported exceptions we get and verify that they
        // match the number we were expecting.
        const actualEx = getShardedViewExceptions(comment);
        for (const ns in expectedExceptions) {
            const actualCount = actualEx[ns] || 0;
            const expectedCount = expectedExceptions[ns];
            assert(actualCount == expectedCount,
                   "expected: " + expectedCount + " exceptions for ns " + ns + ", actually got " +
                       actualCount + " exceptions.");
        }
    }
    testCount++;
}

function checkView(viewName, expected) {
    assertArrayEq({actual: testDB[viewName].find({}).toArray(), expected});
}

function moveChunksByShardKey(collection, shard) {
    assert.commandWorked(testDB.adminCommand({
        moveChunk: collection.getFullName(),
        find: {shard_key: shard},
        to: sharded[shard].shardName
    }));
}

// In order to trigger CommandOnShardedViewNotSupportedOnMongod exceptions where a shard cannot
// resolve a view definition, ensure that:
// - 'local' is backed only by shard 1
// - 'foreign' is backed only by shard 2
// - 'otherForeign' is backed only by shard 3
moveChunksByShardKey(local, "shard1");
moveChunksByShardKey(foreign, "shard2");
moveChunksByShardKey(otherForeign, "shard3");

// Create a view with an empty pipeline on 'local'.
assert.commandWorked(testDB.createView("emptyViewOnLocal", local.getName(), []));
checkView("emptyViewOnLocal", localDocs);

// Create a view with an empty pipeline on 'foreign'.
assert.commandWorked(testDB.createView("emptyViewOnForeign", foreign.getName(), []));
checkView("emptyViewOnForeign", foreignDocs);

// Create a view with an empty pipeline on 'otherForeign'.
assert.commandWorked(testDB.createView("emptyViewOnOtherForeign", otherForeign.getName(), []));
checkView("emptyViewOnOtherForeign", otherForeignDocs);

// Create a view with a pipeline containing only a $match stage on 'foreign'.
assert.commandWorked(testDB.createView("simpleMatchView", foreign.getName(), [{$match: {f: "b"}}]));
checkView("simpleMatchView", [
    {join_field: 2, _id: 5, shard_key: "shard2", f: "b"},
]);

// Create a view with a slightly more interesting pipeline on 'foreign'.
assert.commandWorked(testDB.createView("projectMatchView", foreign.getName(), [
    {$project: {join_field: 1, _id: 1, sum: {$add: ["$_id", "$join_field"]}}},
    {$match: {sum: {$gt: 5}}},
]));
checkView("projectMatchView", [
    {join_field: 2, _id: 5, sum: 7},
    {join_field: 3, _id: 6, sum: 9},
]);

// Create a view on 'foreign' whose pipeline contains a $lookup on collection 'local'.
assert.commandWorked(testDB.createView(
    "viewOnForeignWithEmptyLookupOnLocal", foreign.getName(), [
        {$match: {f: "b"}},
        {$lookup: {
            from: "emptyViewOnLocal",
            pipeline: [],
            as: "local",
        }}
    ]));
checkView("viewOnForeignWithEmptyLookupOnLocal", [
    {
        join_field: 2,
        _id: 5,
        shard_key: "shard2",
        f: "b",
        local: [
            {_id: 1, shard_key: "shard1", f: 1},
            {_id: 2, shard_key: "shard1", f: 2},
            {_id: 3, shard_key: "shard1", f: 3},
        ]
    },
]);

assert.commandWorked(testDB.createView(
    "viewOnForeignWithPipelineLookupOnLocal", foreign.getName(), [
        {$match: {f: "b"}},
        {$lookup: {
            from: "emptyViewOnLocal",
            pipeline: [
                {$match: {f: 1}},
            ],
            as: "local",
        }},
        {$unwind: "$local"},
    ]));
checkView("viewOnForeignWithPipelineLookupOnLocal", [
    {
        join_field: 2,
        _id: 5,
        shard_key: "shard2",
        f: "b",
        local: {_id: 1, shard_key: "shard1", f: 1},
    },
]);

// Create a view whose pipeline contains a $lookup on a sharded view with fields to join on.
assert.commandWorked(testDB.createView(
    "viewOnForeignWithJoinLookupOnLocal", foreign.getName(), [
        {$match: {f: "b"}},
        {$lookup: {
            from: "emptyViewOnLocal",
            localField: "join_field",
            foreignField: "_id",
            as: "local",
        }}
    ]));
checkView("viewOnForeignWithJoinLookupOnLocal", [
    {
        join_field: 2,
        _id: 5,
        shard_key: "shard2",
        f: "b",
        local: [
            {_id: 2, shard_key: "shard1", f: 2},
        ]
    },
]);

// Verify that we can resolve views containing a top-level $lookup targeted to other non-primary
// shards.
assert.commandWorked(testDB.createView(
    "viewOnForeignWithLookupOnOtherForeign", foreign.getName(), [
        {$match: {f: "b"}},
        {$lookup: {
            from: "emptyViewOnOtherForeign",
            pipeline: [
                {$match: {_id: 7}},
            ],
            as: "otherForeign",
        }},
        {$unwind: "$otherForeign"}
    ]));
checkView("viewOnForeignWithLookupOnOtherForeign", [
    {
        join_field: 2,
        _id: 5,
        shard_key: "shard2",
        f: "b",
        otherForeign: {_id: 7, shard_key: "shard3"},
    },
]);

// Verify that we can resolve views containing nested $lookups targeted to other non-primary shards.
assert.commandWorked(testDB.createView(
    "viewOnForeignWithLookupOnOtherForeignAndLocal", foreign.getName(), [
        {$match: {f: "b"}},
        {$lookup: {
            from: "emptyViewOnOtherForeign",
            pipeline: [
                {$match: {_id: 7}},
                {$lookup: {
                    from: "emptyViewOnLocal",
                    pipeline: [
                        {$match: {_id: 2}},
                    ],
                    as: "local",
                }},
                {$unwind: "$local"}
            ],
            as: "otherForeign",
        }},
        {$unwind: "$otherForeign"}
    ]));
checkView("viewOnForeignWithLookupOnOtherForeignAndLocal", [
    {
        join_field: 2,
        _id: 5,
        shard_key: "shard2",
        f: "b",
        otherForeign: {_id: 7, shard_key: "shard3", local: {_id: 2, shard_key: "shard1", f: 2}},
    },
]);

// TODO: SERVER-59911. After SERVER-59501, in the following queries the $lookup is sent to the
// primary, which can resolve the foreign views without triggering sharded view exceptions. If it is
// possible to parallelize $lookup when the foreign is a sharded view, the below queries should
// trigger a non-zero number of exceptions.

// Test that sharded view resolution works correctly with empty pipelines.
testLookupView({
    pipeline: [
        {$lookup: {
            from: "emptyViewOnForeign",
            pipeline: [],
            as: "foreign",
        }}
    ],
    expectedResults: [
        {_id: 1, f: 1, shard_key: "shard1", foreign: [
            {join_field: 1, _id: 4, f: "a", shard_key: "shard2"},
            {join_field: 2, _id: 5, f: "b", shard_key: "shard2"},
            {join_field: 3, _id: 6, f: "c", shard_key: "shard2"},
        ]},
        {_id: 2, f: 2, shard_key: "shard1", foreign: [
            {join_field: 1, _id: 4, f: "a", shard_key: "shard2"},
            {join_field: 2, _id: 5, f: "b", shard_key: "shard2"},
            {join_field: 3, _id: 6, f: "c", shard_key: "shard2"},
        ]},
        {_id: 3, f: 3, shard_key: "shard1", foreign: [
            {join_field: 1, _id: 4, f: "a", shard_key: "shard2"},
            {join_field: 2, _id: 5, f: "b", shard_key: "shard2"},
            {join_field: 3, _id: 6, f: "c", shard_key: "shard2"},
        ]},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 0}
});

// Test that sharded view resolution works correctly with empty pipelines and a join field.
testLookupView({
    pipeline: [
        {$lookup: {
            from: "emptyViewOnForeign",
            localField: "_id",
            foreignField: "join_field",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"}
    ],
    expectedResults: [
        {_id: 1, f: 1, shard_key: "shard1", foreign: {join_field: 1, _id: 4, f: "a", shard_key: "shard2"}},
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 2, _id: 5, f: "b", shard_key: "shard2"}},
        {_id: 3, f: 3, shard_key: "shard1", foreign: {join_field: 3, _id: 6, f: "c", shard_key: "shard2"}},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 0}
});

// Test that sharded view resolution works correctly with a simple view and a join field.
testLookupView({
    pipeline: [
        {$lookup: {
            from: "simpleMatchView",
            localField: "_id",
            foreignField: "join_field",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"}
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 2, _id: 5, f: "b", shard_key: "shard2"}},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 0}
});

// Test that sharded view resolution works correctly with a simple view.

testLookupView({
    pipeline: [
        {$match: {f: 2}},
        {$lookup: {
            from: "simpleMatchView",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"}
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 2, _id: 5, f: "b", shard_key: "shard2"}},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 0}
});

testLookupView({
    pipeline: [
        {$match: {f: 2}},
        {$lookup: {
            from: "projectMatchView",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"},
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 2, _id: 5, sum: 7}},
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 3, _id: 6, sum: 9}},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 0}
});

testLookupView({
    pipeline: [
        {$match: {f: 2}},
        {$lookup: {
            from: "projectMatchView",
            pipeline: [
                {$project: {sum: 1}},
            ],
            as: "foreign",
        }},
        {$addFields: {sum: {$reduce: {input: "$foreign", initialValue: 0, in: {$add: ["$$value", "$$this.sum"]}}}}},
        {$project: {_id: 1, sum: 1}},
    ],
    expectedResults: [
        {_id: 2, sum: 16},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 0}
});

testLookupView({
    pipeline: [
        {$match: {_id: 2}},
        {$lookup: {
            from: "viewOnForeignWithLookupOnOtherForeign",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"}
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign: {
            join_field: 2, _id: 5, shard_key: "shard2", f: "b", otherForeign:
                {_id: 7, shard_key: "shard3"},
        }},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 1}
});

testLookupView({
    pipeline: [
        {$match: {_id: 3}},
        {$lookup: {
            from: "viewOnForeignWithLookupOnOtherForeignAndLocal",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"}
    ],
    expectedResults: [
        {_id : 3, shard_key : "shard1", f : 3, foreign :
            {
                _id : 5,
                join_field : 2,
                shard_key : "shard2",
                f : "b",
                otherForeign : {
                        _id : 7,
                        shard_key : "shard3",
                        local : {
                                _id : 2,
                                shard_key : "shard1",
                                f : 2
                        }
                }
            }
        }
    ],
    expectedExceptions: {"test.local": 1, "test.foreign": 1, "test.otherForeign": 1}
});

testLookupView({
    pipeline: [
        {$match: {f: 2}},
        {$lookup: {
            from: "viewOnForeignWithEmptyLookupOnLocal",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"},
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 2, _id: 5, f: "b",
        shard_key: "shard2", local: [
            {_id: 1, f: 1, shard_key: "shard1"},
            {_id: 2, f: 2, shard_key: "shard1"},
            {_id: 3, f: 3, shard_key: "shard1"},
        ]}},
    ],
    expectedExceptions: {"test.local": 1, "test.foreign": 1, "test.otherForeign": 0}
});

// Test that sharded view resolution works correctly with a view pipeline containing a $lookup with
// a join field.
testLookupView({
    pipeline: [
        {$match: {f: 2}},
        {$lookup: {
            from: "viewOnForeignWithEmptyLookupOnLocal",
            localField: "_id",
            foreignField: "join_field",
            as: "foreign",
        }},
        {$unwind: "$foreign"},
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 2, _id: 5, f: "b",
        shard_key: "shard2", local: [
            {_id: 1, f: 1, shard_key: "shard1"},
            {_id: 2, f: 2, shard_key: "shard1"},
            {_id: 3, f: 3, shard_key: "shard1"},
        ]}},
    ],
    expectedExceptions: {"test.local": 1, "test.foreign": 1, "test.otherForeign": 0}
});

// Test that sharded view resolution works correctly with a view pipeline containing a $lookup and a
// join field.
testLookupView({
    pipeline: [
        {$match: {f: 2}},
        {$lookup: {
            from: "viewOnForeignWithJoinLookupOnLocal",
            pipeline: [
                {$unwind: "$local"},
                {$match: {$expr: {$eq: ["$join_field", "$local.f"]}}}
            ],
            as: "foreign",
        }},
        {$unwind: "$foreign"}
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign: {join_field: 2, _id: 5, f: "b", shard_key:
        "shard2", local: {_id: 2, f: 2, shard_key: "shard1"}}}
    ],
    expectedExceptions: {"test.local": 1, "test.foreign": 1, "test.otherForeign": 0}
});

// Test that sharded view resolution works correctly with a $lookup on a view whose pipeline
// contains another $lookup.
testLookupView({
    pipeline: [
        {$match: {f: 2}},
        {$lookup: {
            from: "viewOnForeignWithPipelineLookupOnLocal",
            pipeline: [],
            as: "foreign",
        }},
        {$unwind: "$foreign"}
    ],
    expectedResults: [
        {_id: 2, f: 2, shard_key: "shard1", foreign:
            {join_field: 2, _id: 5, f: "b", shard_key: "shard2", local: {_id: 1, f: 1, shard_key:
            "shard1"}}
        },
    ],
    expectedExceptions: {"test.local": 1, "test.foreign": 1, "test.otherForeign": 0}
});

// Test that $lookup with a subpipeline containing a non-correlated pipeline prefix can still use
// the cache after sharded view resolution.
const shard1DB = sharded.shard1.getDB(testDBName);
const shard2DB = sharded.shard2.getDB(testDBName);

assert.commandWorked(shard1DB.setProfilingLevel(2));
assert.commandWorked(shard2DB.setProfilingLevel(2));

// Add a chunk of the 'foreign' collection to shard1 (the rest is on shard2) to force a merge.
assert.commandWorked(foreign.insert({_id: 9, shard_key: "shard1", f: "d"}));
assert.commandWorked(
    testDB.adminCommand({split: foreign.getFullName(), find: {shard_key: "shard1"}}));
moveChunksByShardKey(foreign, "shard1");

testLookupView({
    pipeline: [{$lookup: {
        from: "emptyViewOnForeign",
        let: {localId: "$_id"},
        pipeline: [{$group: {_id: {oddId: {$mod: ["$_id", 2]}}, f: {$addToSet: "$f"}}}, {$match: {$expr: {$eq: ["$_id.oddId", {$mod: ["$$localId", 2]}]}}}],
        as: "foreign",
    }}],
    expectedResults: [
        {_id: 1, shard_key: "shard1", f: 1, foreign: [{_id: {oddId: 1}, f: ["b", "d"]}]},
        {_id: 2, shard_key: "shard1", f: 2, foreign: [{_id: {oddId: 0}, f: ["a", "c"]}]},
        {_id: 3, shard_key: "shard1", f: 3, foreign: [{_id: {oddId: 1}, f: ["b", "d"]}]},
    ],
    expectedExceptions: {"test.local": 0, "test.foreign": 1, "test.otherForeign": 0}
});

const comment = "test " + (testCount - 1);

// The subpipeline only executes once on each of the shards containing the 'foreign' collection.
profilerHasSingleMatchingEntryOrThrow({
    profileDB: shard1DB,
    filter: {
        "command.aggregate": foreign.getName(),
        "command.comment": comment,
        "command.fromMongos": false,
        "errMsg": {$exists: false}  // For the StaleConfig error that resulted from moveChunk.
    }
});

profilerHasSingleMatchingEntryOrThrow({
    profileDB: shard2DB,
    filter: {
        "command.aggregate": foreign.getName(),
        "command.comment": comment,
        "command.fromMongos": false,
        "errMsg": {$exists: false}  // For the StaleConfig error that resulted from moveChunk.
    }
});

sharded.stop();
}());
