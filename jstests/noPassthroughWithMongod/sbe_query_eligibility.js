/**
 * Test that verifies which query shapes which are eligible for SBE.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled'.

/**
 * Utility which asserts whether, when aggregating over 'collection' with 'pipeline', whether
 * explain reports that SBE or classic was used, based on the value of 'isSBE'.
 */
function assertEngineUsed(collection, pipeline, isSBE) {
    const explain = collection.explain().aggregate(pipeline);
    const expectedExplainVersion = isSBE ? "2" : "1";
    assert(explain.hasOwnProperty("explainVersion"), explain);
    assert.eq(explain.explainVersion, expectedExplainVersion, explain);
}

if (!checkSBEEnabled(db)) {
    jsTestLog("Skipping test because SBE is disabled");
    return;
}

const collName = "sbe_eligiblity";
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({}));
assert.eq(coll.find().itcount(), 1);

// Simple eligible cases.
const expectedSbeCases = [
    // Non-$expr match filters.
    [{$match: {a: 1}}],
    [{$match: {"a.b.c": 1}}],

    // Top level projections.
    [{$project: {a: 1, b: 1}}],
    [{$project: {a: 0, b: 0}}],

    // Sorts with no common prefixes.
    [{$sort: {a: 1}}],
    [{$sort: {"a.b.c": 1}}],

    // Test a combination of the above categories.
    [{$match: {a: 1}}, {$sort: {b: 1}}],
    [{$match: {a: 1}}, {$sort: {b: 1, c: 1}}],
    [{$match: {a: 1}}, {$project: {b: 1, c: 1}}, {$sort: {b: 1, c: 1}}],
    [
        {$match: {a: 1, b: 1, "c.d.e": {$mod: [2, 0]}}},
        {$project: {b: 1, c: 1}},
        {$sort: {b: 1, c: 1}}
    ],

    // $lookup and $group should work as expected.
    [
        {$match: {a: 1}},
        {$project: {a: 1}},
        {$lookup: {from: collName, localField: "a", foreignField: "a", as: "out"}}
    ],
    [{$match: {a: 1}}, {$project: {a: 1}}, {$group: {_id: "$a", out: {$sum: 1}}}],

    // If we have a non-SBE compatible expression after the pushdown boundary, this should not
    // inhibit the pushdown of the pipeline prefix into SBE.
    [
        {$match: {a: 1}},
        {$project: {a: 1}},
        {$lookup: {from: collName, localField: "a", foreignField: "a", as: "out"}},
        {$addFields: {foo: {$sum: "$a"}}},
        {$match: {$expr: {$eq: ["$a", "$b"]}}}
    ],
    [
        {$match: {a: 1}},
        {$project: {a: 1}},
        {$group: {_id: "$a", out: {$sum: 1}}},
        {$match: {$expr: {$eq: ["$a", "$b"]}}}
    ],
];

for (const pipeline of expectedSbeCases) {
    assertEngineUsed(coll, pipeline, true /* isSBE */);
}

// The cases below are expected to use SBE only if 'featureFlagSbeFull' is on.
const isSbeFullyEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]);
const sbeFullCases = [
    // Match filters with $expr.
    [{$match: {$expr: {$eq: ["$a", "$b"]}}}],
    [{$match: {$and: [{$expr: {$eq: ["$a", "$b"]}}, {c: 1}]}}],

    // Dotted projections.
    [{$project: {"a.b": 1}}],
    [{$project: {"a.b": 0}}],
    [{$project: {"a.b": 1, "a.c": 1}}],
    [{$project: {"a.b.c.d.e.f.g": 0, "h.i.j.k": 0}}],

    // Computed projections.
    [{$project: {a: {$add: ["$foo", "$bar"]}}}],
    [{$project: {a: {$divide: ["$foo", "$bar"]}}}],

    // Sorts with common prefixes.
    [{$sort: {"a.b": 1, "a.c": 1}}],
    [{$sort: {"a.b.f.g": 1, "a.d.e.f": 1}}],
    [{$sort: {"a": 1, "b": 1, "c.d": 1, "c.f": 1}}],

    // Mix SBE-eligible and non-SBE eligible filters, projections and sorts.

    // Match filters with $expr should inhibit pushdown.
    [{$project: {a: 1, b: 1}}, {$match: {$expr: {$eq: ["$a", "$b"]}}}],
    [{$match: {$and: [{$expr: {$eq: ["$a", "$b"]}}, {c: 1}]}}, {$sort: {a: 1, d: 1}}],
    [
        {$match: {$and: [{$expr: {$eq: ["$a", "$b"]}}, {c: 1}]}},
        {
            $lookup:
                {from: collName, localField: "c_custkey", foreignField: "o_custkey", as: "custsale"}
        }
    ],

    // Dotted projections should inhibit pushdown.
    [{$match: {d: 1}}, {$project: {"a.b": 1}}],
    [{$sort: {d: 1, e: 1}}, {$project: {"a.b": 0}}],
    [{$match: {$or: [{a: {$gt: 0}}, {b: {$gt: 0}}]}}, {$project: {"d.a": 1}}],

    [
        {$project: {"a.b": 1, "a.d": 1}},
        {$lookup: {from: collName, localField: "a", foreignField: "a", as: "out"}},
    ],

    // Computed projections should inhibit pushdown.
    [{$match: {foo: {$gt: 0}}}, {$project: {a: {$add: ["$foo", "$bar"]}}}],
    [{$project: {a: {$add: ["$foo", "$bar"]}}}, {$sort: {a: 1}}],
    [
        {$project: {a: {$add: ["$foo", "$bar"]}}},
        {$group: {_id: "$a", "ct": {$sum: 1}}},
    ],
    [
        {$project: {a: {$add: ["$out", 1]}}},
        {$lookup: {from: collName, localField: "a", foreignField: "a", as: "out"}},
    ],

    // Sorts with common prefixes should inhibit pushdown.
    [{$match: {foo: {$gt: 0}}}, {$sort: {"a.b": 1, "a.c": 1}}],
    [{$sort: {"a.b.f.g": 1, "a.d.e.f": 1}}, {$project: {a: 1}}],
    [{$match: {$or: [{a: {$gt: 0}}, {b: {$gt: 0}}]}}, {$sort: {"d.a": 1, "d.b": 1}}],
    [
        {$sort: {"a.b": 1, "a.c": 1}},
        {$group: {_id: {a: "$foo", b: "$bar"}, "a": {$sum: 1}}},
    ],
    [
        {$sort: {"b.c": 1, "b.d": 1}},
        {$lookup: {from: collName, localField: "a", foreignField: "a", as: "out"}},
    ],

    // TPC-H query whose $lookup is SBE compatible, but which features a $match which uses $expr.
    // Note that $match will be pushed to the front of the pipeline.
    [
        {
            $lookup:
                {from: collName, localField: "c_custkey", foreignField: "o_custkey", as: "custsale"}
        },
        {$addFields: {cntrycode: {$substr: ["$c_phone", 0, 2]}, custsale: {$size: "$custsale"}}},
        {
            $match: {
                $and: [
                    {
                        $expr: {
                            $in: [
                                "$cntrycode",
                                [
                                    {$toString: "13"},
                                    {$toString: "31"},
                                    {$toString: "23"},
                                    {$toString: "29"},
                                    {$toString: "30"},
                                    {$toString: "18"},
                                    {$toString: "17"}
                                ]
                            ]
                        }
                    },
                    {$expr: {$gt: ["$c_acctbal", 0.00]}}
                ]
            }
        }
    ],
];

for (const pipeline of sbeFullCases) {
    assertEngineUsed(coll, pipeline, isSbeFullyEnabled /* isSBE */);
}
})();
