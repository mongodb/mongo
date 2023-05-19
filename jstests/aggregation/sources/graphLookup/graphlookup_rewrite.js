// Tests that stage rewrite optimizations for $graphLookup work correctly.
//
// This test makes assumptions about how the explain output will be formatted.
// @tags: [
//  assumes_unsharded_collection,
//  do_not_wrap_aggregations_in_facets,
//  requires_pipeline_optimization,
// ]
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');
load("jstests/libs/analyze_plan.js");
load("jstests/libs/fixture_helpers.js");

const coll = db.graphlookup_rewrite;
coll.drop();

assert.commandWorked(coll.insertMany([
    {"_id": 1, "from": "a", "foo": 1},
    {"_id": 2, "from": "b", "to": "a", "foo": 2},
    {"_id": 3, "from": "c", "to": "b", "foo": 3},
    {"_id": 4, "from": "d", "to": "b", "foo": 4},
    {"_id": 5, "from": "e", "to": "c", "foo": 5},
    {"_id": 6, "from": "f", "to": "d", "foo": 6}
]));

function assertStagesAndOutput({
    pipeline = [],
    expectedStages = [],
    optimizedAwayStages = [],
    expectedOutput = [],
    orderedArrayComparison = true,
    fieldsToSkip = [],
    msg = ""
}) {
    const explain = coll.explain().aggregate(pipeline);
    const output = coll.aggregate(pipeline).toArray();

    for (const stage of expectedStages) {
        assert(aggPlanHasStage(explain, stage),
               `${msg}: missing stage ${stage}: ${tojson(explain)}`);
    }
    for (const stage of optimizedAwayStages) {
        assert(!aggPlanHasStage(explain, stage),
               `${msg}: stage ${stage} not optimized away: ${tojson(explain)}`);
    }

    const res = orderedArrayComparison
        ? orderedArrayEq(output, expectedOutput, false, fieldsToSkip)
        : arrayEq(output, expectedOutput, false, null /*valueComparator*/, fieldsToSkip);
    assert(res, `actual=${tojson(output)}, expected=t${tojson(expectedOutput)}`);
}

const graphLookup = {
    $graphLookup: {
        from: "graphlookup_rewrite",
        startWith: "$from",
        connectFromField: "from",
        connectToField: "to",
        as: "out"
    }
};

assertStagesAndOutput({
    pipeline: [graphLookup, {$sort: {foo: 1}}],
    expectedStages: ["SORT", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["$sort"],
    expectedOutput: [
        {
            "_id": 1,
            "from": "a",
            "foo": 1,
            "out": [
                {"_id": 2, "from": "b", "to": "a", "foo": 2},
                {"_id": 3, "from": "c", "to": "b", "foo": 3},
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 4, "from": "d", "to": "b", "foo": 4}
            ]
        },
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": [
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 3, "from": "c", "to": "b", "foo": 3},
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 4, "from": "d", "to": "b", "foo": 4}
            ]
        },
        {
            "_id": 3,
            "from": "c",
            "to": "b",
            "foo": 3,
            "out": [{"_id": 5, "from": "e", "to": "c", "foo": 5}]
        },
        {
            "_id": 4,
            "from": "d",
            "to": "b",
            "foo": 4,
            "out": [{"_id": 6, "from": "f", "to": "d", "foo": 6}]
        },
        {"_id": 5, "from": "e", "to": "c", "foo": 5, "out": []},
        {"_id": 6, "from": "f", "to": "d", "foo": 6, "out": []}
    ],
    msg: "$graphLookup should swap with $sort if there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$limit: 100}],
    expectedStages: ["LIMIT", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["$limit"],
    orderedArrayComparison: false,
    expectedOutput: [
        {
            "_id": 1,
            "from": "a",
            "foo": 1,
            "out": [
                {"_id": 2, "from": "b", "to": "a", "foo": 2},
                {"_id": 3, "from": "c", "to": "b", "foo": 3},
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 4, "from": "d", "to": "b", "foo": 4}
            ]
        },
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": [
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 3, "from": "c", "to": "b", "foo": 3},
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 4, "from": "d", "to": "b", "foo": 4}
            ]
        },
        {
            "_id": 3,
            "from": "c",
            "to": "b",
            "foo": 3,
            "out": [{"_id": 5, "from": "e", "to": "c", "foo": 5}]
        },
        {
            "_id": 4,
            "from": "d",
            "to": "b",
            "foo": 4,
            "out": [{"_id": 6, "from": "f", "to": "d", "foo": 6}]
        },
        {"_id": 5, "from": "e", "to": "c", "foo": 5, "out": []},
        {"_id": 6, "from": "f", "to": "d", "foo": 6, "out": []}
    ],
    msg: "$graphLookup should swap with $limit if there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$skip: 100}],
    expectedStages: ["SKIP", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["$skip"],
    expectedOutput: [],
    msg: "$graphLookup should swap with $skip if there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$sort: {foo: 1}}, {$limit: 100}],
    expectedStages: ["SORT", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["LIMIT", "$limit"],
    expectedOutput: [
        {
            "_id": 1,
            "from": "a",
            "foo": 1,
            "out": [
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 2, "from": "b", "to": "a", "foo": 2},
                {"_id": 4, "from": "d", "to": "b", "foo": 4},
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 3, "from": "c", "to": "b", "foo": 3}
            ]
        },
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": [
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 4, "from": "d", "to": "b", "foo": 4},
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 3, "from": "c", "to": "b", "foo": 3}
            ]
        },
        {
            "_id": 3,
            "from": "c",
            "to": "b",
            "foo": 3,
            "out": [{"_id": 5, "from": "e", "to": "c", "foo": 5}]
        },
        {
            "_id": 4,
            "from": "d",
            "to": "b",
            "foo": 4,
            "out": [{"_id": 6, "from": "f", "to": "d", "foo": 6}]
        },
        {"_id": 5, "from": "e", "to": "c", "foo": 5, "out": []},
        {"_id": 6, "from": "f", "to": "d", "foo": 6, "out": []}
    ],
    msg: "$graphLookup should swap with $limit and $sort, and $sort should absorb $limit if " +
        "there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$sort: {out: 1, foo: 1}}],
    expectedStages: ["COLLSCAN", "$graphLookup", "$sort"],
    expectedOutput: [
        {"_id": 5, "from": "e", "to": "c", "foo": 5, "out": []},
        {"_id": 6, "from": "f", "to": "d", "foo": 6, "out": []},
        {
            "_id": 1,
            "from": "a",
            "foo": 1,
            "out": [
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 2, "from": "b", "to": "a", "foo": 2},
                {"_id": 4, "from": "d", "to": "b", "foo": 4},
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 3, "from": "c", "to": "b", "foo": 3}
            ]
        },
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": [
                {"_id": 6, "from": "f", "to": "d", "foo": 6},
                {"_id": 4, "from": "d", "to": "b", "foo": 4},
                {"_id": 5, "from": "e", "to": "c", "foo": 5},
                {"_id": 3, "from": "c", "to": "b", "foo": 3}
            ]
        },
        {
            "_id": 3,
            "from": "c",
            "to": "b",
            "foo": 3,
            "out": [{"_id": 5, "from": "e", "to": "c", "foo": 5}]
        },
        {
            "_id": 4,
            "from": "d",
            "to": "b",
            "foo": 4,
            "out": [{"_id": 6, "from": "f", "to": "d", "foo": 6}]
        }
    ],
    msg: "$graphLookup should not swap with $sort if sort uses fields created by $graphLookup"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$unwind: "$out"}, {$sort: {foo: 1}}],
    expectedStages: ["COLLSCAN", "$graphLookup", "$sort"],
    expectedOutput: [
        {"_id": 1, "from": "a", "foo": 1, "out": {"_id": 6, "from": "f", "to": "d", "foo": 6}},
        {"_id": 1, "from": "a", "foo": 1, "out": {"_id": 2, "from": "b", "to": "a", "foo": 2}},
        {"_id": 1, "from": "a", "foo": 1, "out": {"_id": 4, "from": "d", "to": "b", "foo": 4}},
        {"_id": 1, "from": "a", "foo": 1, "out": {"_id": 5, "from": "e", "to": "c", "foo": 5}},
        {"_id": 1, "from": "a", "foo": 1, "out": {"_id": 3, "from": "c", "to": "b", "foo": 3}},
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": {"_id": 6, "from": "f", "to": "d", "foo": 6}
        },
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": {"_id": 4, "from": "d", "to": "b", "foo": 4}
        },
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": {"_id": 5, "from": "e", "to": "c", "foo": 5}
        },
        {
            "_id": 2,
            "from": "b",
            "to": "a",
            "foo": 2,
            "out": {"_id": 3, "from": "c", "to": "b", "foo": 3}
        },
        {
            "_id": 3,
            "from": "c",
            "to": "b",
            "foo": 3,
            "out": {"_id": 5, "from": "e", "to": "c", "foo": 5}
        },
        {
            "_id": 4,
            "from": "d",
            "to": "b",
            "foo": 4,
            "out": {"_id": 6, "from": "f", "to": "d", "foo": 6}
        }
    ],
    msg: "$graphLookup with an internal $unwind should not swap with $sort",
    fieldsToSkip: ["out"]
});
})();
