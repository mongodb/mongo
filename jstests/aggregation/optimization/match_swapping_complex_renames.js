/**
 * Tests the behavior of $match pushdown over complex renames.
 *
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 *   featureFlagImprovedDepsAnalysis,
 *   # The test asserts on explain output.
 *   assumes_unsharded_collection,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";

const coll = db.match_swapping_complex_renames;

function createIndexes(indexes) {
    if (Array.isArray(indexes)) {
        indexes.forEach((idx) => assert.commandWorked(coll.createIndex(idx)));
    } else {
        assert.commandWorked(coll.createIndex(indexes));
    }
}

function runTest({name, pipeline, positive, negative}) {
    describe(name, function () {
        if (positive) {
            it("pushes down $match", function () {
                coll.drop();
                assert.commandWorked(coll.insertMany(positive.docs));
                createIndexes(positive.index);
                assert.eq(positive.expectedCount, coll.aggregate(pipeline).itcount());
                const explain = coll.explain().aggregate(pipeline);
                assert.neq(null, getAggPlanStage(explain, "IXSCAN"), "Expected IXSCAN (pushdown): " + tojson(explain));
            });
        }

        if (negative) {
            it("does not push down $match", function () {
                coll.drop();
                assert.commandWorked(coll.insertMany(negative.docs));
                createIndexes(negative.index);
                assert.eq(negative.expectedCount, coll.aggregate(pipeline).itcount());
                const explain = coll.explain().aggregate(pipeline);
                assert.eq(null, getAggPlanStage(explain, "IXSCAN"), "Expected NO IXSCAN: " + tojson(explain));
            });
        }
    });
}

runTest({
    name: "$addFields complex rename {a: '$b.c'}",
    pipeline: [{$addFields: {a: "$b.c"}}, {$match: {a: 42}}],
    positive: {
        docs: [
            {b: {c: 42}, d: 1},
            {b: {c: 99}, d: 2},
            {b: {c: 42}, d: 3},
        ],
        index: {"b.c": 1},
        expectedCount: 2,
    },
    negative: {
        docs: [{b: [{c: 42}]}, {b: [{c: 99}]}, {b: [{c: 42}, {c: 1}]}],
        index: {"b.c": 1},
        expectedCount: 2,
    },
});

runTest({
    name: "$project complex rename {a: '$b.c'}",
    pipeline: [{$project: {a: "$b.c", _id: 1}}, {$match: {a: {$gte: 42}}}],
    positive: {
        docs: [
            {b: {c: 42}, d: 1},
            {b: {c: 99}, d: 2},
        ],
        index: {"b.c": 1},
        expectedCount: 2,
    },
    negative: {
        docs: [{b: [{c: 42}]}, {b: [{c: 99}]}],
        index: {"b.c": 1},
        expectedCount: 2,
    },
});

runTest({
    name: "$set complex rename {a: '$b.c'}",
    pipeline: [{$set: {a: "$b.c"}}, {$match: {a: 42}}],
    positive: {
        docs: [{b: {c: 42}}, {b: {c: 99}}, {b: {c: 42}}],
        index: {"b.c": 1},
        expectedCount: 2,
    },
    negative: {
        docs: [{b: [{c: 42}]}, {b: [{c: 99}]}],
        index: {"b.c": 1},
        expectedCount: 1,
    },
});

runTest({
    name: "$addFields deeper path {a: '$b.c.d'}",
    pipeline: [{$addFields: {a: "$b.c.d"}}, {$match: {a: 42}}],
    positive: {
        docs: [{b: {c: {d: 42}}}, {b: {c: {d: 99}}}, {b: {c: {d: 42}}}],
        index: {"b.c.d": 1},
        expectedCount: 2,
    },
    negative: {
        docs: [{b: [{c: {d: 42}}]}, {b: [{c: {d: 99}}]}],
        index: {"b.c.d": 1},
        expectedCount: 1,
    },
});

runTest({
    name: "$addFields depth-4 path {a: '$b.c.d.e'}",
    pipeline: [{$addFields: {a: "$b.c.d.e"}}, {$match: {a: 42}}],
    positive: {
        docs: [{b: {c: {d: {e: 42}}}}, {b: {c: {d: {e: 99}}}}, {b: {c: {d: {e: 42}}}}],
        index: {"b.c.d.e": 1},
        expectedCount: 2,
    },
    negative: {
        docs: [{b: [{c: {d: {e: 42}}}]}, {b: [{c: {d: {e: 99}}}]}],
        index: {"b.c.d.e": 1},
        expectedCount: 1,
    },
});

runTest({
    name: "$addFields depth-5 path {a: '$b.c.d.e.f'}",
    pipeline: [{$addFields: {a: "$b.c.d.e.f"}}, {$match: {a: 42}}],
    positive: {
        docs: [{b: {c: {d: {e: {f: 42}}}}}, {b: {c: {d: {e: {f: 99}}}}}],
        index: {"b.c.d.e.f": 1},
        expectedCount: 1,
    },
    negative: {
        docs: [{b: [{c: {d: {e: {f: 42}}}}]}, {b: [{c: {d: {e: {f: 99}}}}]}],
        index: {"b.c.d.e.f": 1},
        expectedCount: 1,
    },
});

// $set{a:1} establishes "a" as non-array via the dep graph, enabling the left-dotted
// rename {"a.b": "$c"} to be treated as a simple rename. The match is rewritten to {c: 42}.
runTest({
    name: "$set + $addFields left-dotted rename {'a.b': '$c'}",
    pipeline: [{$set: {a: 1}}, {$addFields: {"a.b": "$c"}}, {$match: {"a.b": 42}}],
    positive: {
        docs: [{c: 42}, {c: 99}, {c: 42}],
        index: {c: 1},
        expectedCount: 2,
    },
});

runTest({
    name: "$set constant array + $addFields left-dotted rename {'a.b': '$c'}",
    pipeline: [{$set: {a: [1, 2]}}, {$addFields: {"a.b": "$c"}}, {$match: {"a.b": 42}}],
    negative: {
        docs: [{c: 42}, {c: 99}, {c: 42}],
        index: {c: 1},
        expectedCount: 2,
    },
});

// Without proof that "a" is non-array, the left-dotted rename should not push down.
runTest({
    name: "$addFields left-dotted rename {'a.b': '$c'}",
    pipeline: [{$addFields: {"a.b": "$c"}}, {$match: {"a.b": 42}}],
    negative: {
        docs: [
            {a: [{x: 1}], c: 42},
            {a: [{x: 2}], c: 99},
        ],
        index: {c: 1},
        expectedCount: 1,
    },
});

// $set{a:1} + data with non-array "c" enables both sides to be proven non-array.
runTest({
    name: "$set + $addFields both-dotted rename {'a.b': '$c.d'}",
    pipeline: [{$set: {a: 1}}, {$addFields: {"a.b": "$c.d"}}, {$match: {"a.b": 42}}],
    positive: {
        docs: [{c: {d: 42}}, {c: {d: 99}}, {c: {d: 42}}],
        index: {"c.d": 1},
        expectedCount: 2,
    },
});

// Without proof that "a" is non-array, the both-dotted rename should not push down.
runTest({
    name: "$addFields both-dotted rename {'a.b': '$c.d'}",
    pipeline: [{$addFields: {"a.b": "$c.d"}}, {$match: {"a.b": 42}}],
    negative: {
        docs: [
            {a: [{x: 1}], c: {d: 42}},
            {a: [{x: 2}], c: {d: 99}},
        ],
        index: {"c.d": 1},
        expectedCount: 1,
    },
});

runTest({
    name: "$group + $set rename chain",
    pipeline: [
        {$group: {_id: "$orderDetails"}},
        {$set: {address: "$_id.postalAddress"}},
        {$set: {city: "$address.city"}},
        {$match: {city: "Dublin"}},
    ],
    positive: {
        docs: [
            {orderDetails: {postalAddress: {city: "Dublin"}, zip: "D01"}},
            {orderDetails: {postalAddress: {city: "London"}, zip: "SW1"}},
            {orderDetails: {postalAddress: {city: "Dublin"}, zip: "D02"}},
        ],
        index: {"orderDetails.postalAddress.city": 1},
        expectedCount: 2,
    },
    negative: {
        docs: [
            {orderDetails: {postalAddress: [{city: "Dublin"}], zip: "D01"}},
            {orderDetails: {postalAddress: [{city: "London"}], zip: "SW1"}},
            {orderDetails: {postalAddress: [{city: "Dublin"}], zip: "D02"}},
        ],
        index: {"orderDetails.postalAddress.city": 1},
        expectedCount: 2,
    },
});
