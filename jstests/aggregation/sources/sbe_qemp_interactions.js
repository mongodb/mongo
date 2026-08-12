/**
 * Test correctness of aggregations containing combinations of SBE QEMP stages.
 *
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_getmore,
 *   requires_fcv_90,
 *   requires_pipeline_optimization,
 * ]
 */
import {anyEq} from "jstests/aggregation/extras/utils.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {getEngine} from "jstests/libs/query/analyze_plan.js";

// The purpose of this test is not to assert engine selection works or the
// flags work, so we won't assert precisely what engine is chosen, unless
// it's clear SBE should be used (default flag configuration).
const queriesShouldUseSbe = (function () {
    const frameworkControl = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
    ).internalQueryFrameworkControl;
    if (frameworkControl === "forceClassicEngine") {
        return false;
    }

    const requiredFlags = [
        "GetExecutorDeferredEngineChoice",
        "SbeEqLookupUnwindHashJoin",
        "SbeEqLookupUnwindIndexedLoopJoin",
        "SbeEqLookupUnwindNestedLoopJoin",
        "SbeEqLookupUnwindDynamicIndexedLoopJoin",
        "SbeEqLookupUnwindLocalCollscan",
        "SbeEqLookupUnwindLocalIxscanFetch",
        "SbeEqLookupUnwindLocalComplexDataAccessPlans",
        "SbeNonLeadingMatch",
        "SbeTransformStages",
    ];
    return requiredFlags.every((flag) => FeatureFlagUtil.isEnabled(db, flag));
})();

const baseColl = db[jsTestName() + "_base"];
const collA = db[jsTestName() + "_a"];
const collB = db[jsTestName() + "_b"];

const baseDocs = [
    {_id: 0, a: 1, b: 2, c: 3},
    {_id: 1, a: 4, b: 5, c: 6},
];

// These docs match base.a+base.b==collA.x or base.c==collA.x for the first local
// document but not the second.
const collADoc1Match1 = {_id: 10, x: 3, y: 5, z: 5};
const collADoc1Match2 = {_id: 11, x: 3, y: 5, z: 11};

const collADocs = [collADoc1Match1, collADoc1Match2, {_id: 12, x: 4, y: 7, z: 6}];

// Second foreign source docs for collB.
const collBMatch1 = {_id: 20, s: 3, t: 5};

assert(baseColl.drop());
assert(collA.drop());
assert(collB.drop());
assert.commandWorked(baseColl.insert(baseDocs));
assert.commandWorked(collA.insert(collADocs));
assert.commandWorked(collB.insert(collBMatch1));

const anyCollSharded = [baseColl, collA, collB].some((coll) => FixtureHelpers.isSharded(coll));

function assertPipelineResults(pipeline, expected) {
    // Run explain to make sure it passes and assert SBE is used if all
    // necessary flags are on.
    const explain = baseColl.explain().aggregate(pipeline);
    if (queriesShouldUseSbe && !anyCollSharded && !FixtureHelpers.isMongos(db)) {
        assert.eq(getEngine(explain), "sbe", explain);
        assert(!explain.stages || explain.stages.length === 0, explain);
    }

    const results = baseColl.aggregate(pipeline).toArray();
    assert(anyEq(expected, results), {
        pipeline,
        expected,
        results,
        explain,
    });
}

describe("SBE QEMP stage interactions", function () {
    // ---------- NLM/transform followed by $lookup/$lookup-$unwind ----------
    it("Add a new field `r`, match on it, then perform lookup on it", function () {
        assertPipelineResults(
            [
                {$addFields: {r: {$add: ["$a", "$b"]}}},
                {$match: {r: 3}},
                {$lookup: {from: collA.getName(), localField: "r", foreignField: "x", as: "docs"}},
            ],
            [
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    c: 3,
                    r: 3,
                    docs: [collADoc1Match1, collADoc1Match2],
                },
            ],
        );
    });

    it("Add a new field `r`, match on it, then perform lookup-unwind on it", function () {
        assertPipelineResults(
            [
                {$addFields: {r: {$add: ["$a", "$b"]}}},
                {$match: {r: 3}},
                {$lookup: {from: collA.getName(), localField: "r", foreignField: "x", as: "docs"}},
                {$unwind: "$docs"},
            ],
            [
                {_id: 0, a: 1, b: 2, c: 3, r: 3, docs: collADoc1Match1},
                {_id: 0, a: 1, b: 2, c: 3, r: 3, docs: collADoc1Match2},
            ],
        );
    });
    it("Add a new field `r`, match on it, then perform lookup on a different field", function () {
        assertPipelineResults(
            [
                {$addFields: {r: {$add: ["$a", "$b"]}}},
                {$match: {r: 3}},
                {$lookup: {from: collA.getName(), localField: "c", foreignField: "x", as: "docs"}},
            ],
            [
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    c: 3,
                    r: 3,
                    docs: [collADoc1Match1, collADoc1Match2],
                },
            ],
        );
    });
    it("Add a new field `r`, match on it, then perform lookup-unwind on a different field", function () {
        assertPipelineResults(
            [
                {$addFields: {r: {$add: ["$a", "$b"]}}},
                {$match: {r: 3}},
                {$lookup: {from: collA.getName(), localField: "c", foreignField: "x", as: "docs"}},
                {$unwind: "$docs"},
            ],
            [
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    c: 3,
                    r: 3,
                    docs: collADoc1Match1,
                },
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    c: 3,
                    r: 3,
                    docs: collADoc1Match2,
                },
            ],
        );
    });
    it("Perform lookup on a field, then replace the joined field with the join results and filter on it", function () {
        assertPipelineResults(
            [
                {$lookup: {from: collA.getName(), localField: "c", foreignField: "x", as: "docs"}},
                {$addFields: {c: "$docs"}},
                {$match: {"c.z": 5}},
            ],
            [
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    // The first document has field doc.z == 5.
                    c: [collADoc1Match1, collADoc1Match2],
                    docs: [collADoc1Match1, collADoc1Match2],
                },
            ],
        );
    });
    it("Perform lookup-unwind on a field, then replace the joined field", function () {
        assertPipelineResults(
            [
                {$lookup: {from: collA.getName(), localField: "c", foreignField: "x", as: "docs"}},
                {$unwind: "$docs"},
                {$addFields: {c: "$docs"}},
                // We can't $match on "c" here like the last example, since this would trigger the
                // $lookup-$unwind-$match optimization which prevents SBE from being used.
            ],
            [
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    c: collADoc1Match1,
                    docs: collADoc1Match1,
                },
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    c: collADoc1Match2,
                    docs: collADoc1Match2,
                },
            ],
        );
    });
    it("Perform lookup-unwind on a field, then rename the joined field and group", function () {
        assertPipelineResults(
            [
                {$lookup: {from: collA.getName(), localField: "c", foreignField: "x", as: "docs"}},
                {$unwind: "$docs"},
                // $group produces a $project to remove unnecessary fields, which can be pushed down
                // all the way to the find layer and prevent $LU from using SBE. Adding a $$ROOT
                // projection prevents it from knowing which fields are needed and a $project is not
                // generated.
                {$addFields: {c: "$docs", docSize: {$bsonSize: "$$ROOT"}}},
                // This $group references the join result indirectly, through "c" rather than "docs".
                {
                    $group: {
                        _id: "$a",
                        sum_z: {$sum: "$c.z"},
                        sum_b: {$sum: "$b"},
                        total_size: {$sum: "$docSize"},
                    },
                },
            ],
            [
                {
                    _id: 1,
                    sum_z: 16,
                    sum_b: 4,
                    total_size: 216,
                },
            ],
        );
    });
    // ---------- Consecutive $lookup-$unwinds ----------
    it("Perform lookup on a field, then a lookup that is independent from the results of the first join, then add a new field dependent on both results and filter on that field", function () {
        assertPipelineResults(
            [
                {
                    $lookup: {
                        from: collA.getName(),
                        localField: "c",
                        foreignField: "x",
                        as: "result1",
                    },
                },
                // At this point we have:
                //    {_id: 0, a: 1, b: 2, c: 3, result1: [collADoc1Match1, collADoc1Match2]}
                {
                    $lookup: {
                        from: collB.getName(),
                        // There is one match for collB.s==3
                        localField: "c",
                        foreignField: "s",
                        as: "result2",
                    },
                },
                // Now we have:
                //   {_id: 0, a: 1, b: 2, c: 3, result1: [collADoc1Match1, collADoc1Match2], result2: [collBMatch1]}
                {
                    $addFields: {
                        totalNumResults: {$add: [{$size: "$result1"}, {$size: "$result2"}]},
                    },
                },
                {$match: {totalNumResults: 3}},
            ],
            [
                {
                    _id: 0,
                    a: 1,
                    b: 2,
                    c: 3,
                    result1: [collADoc1Match1, collADoc1Match2],
                    result2: [collBMatch1],
                    totalNumResults: 3,
                },
            ],
        );
    });
    it("Perform lookup-unwind on a field, then a lookup-unwind on the result of the first join, then add a field dependent on the result of both joins", function () {
        assertPipelineResults(
            [
                {
                    $lookup: {
                        from: collA.getName(),
                        localField: "c",
                        foreignField: "x",
                        as: "result1",
                    },
                },
                {$unwind: "$result1"},
                {
                    $lookup: {
                        from: collB.getName(),
                        // Only collADoc1Match1.z has a match to `t` in collB, so we'll have one
                        // document after the $lookup-$unwind.
                        localField: "result1.z",
                        foreignField: "t",
                        as: "result2",
                    },
                },
                {$unwind: "$result2"},
                // Now we have:
                //    {_id: 0, a: 1, b: 2, c: 3, result1: collADoc1Match1, result2: collBMatch1}
                {$addFields: {sum: {$add: ["$result1.y", "$result2.s"]}}},
            ],
            [{_id: 0, a: 1, b: 2, c: 3, result1: collADoc1Match1, result2: collBMatch1, sum: 8}],
        );
    });
    // ---------- $group followed by $lookup/$LU ----------
    it("Group on a field, then lookup on that field", function () {
        assertPipelineResults(
            [
                // Sum of `a` in the base coll is 5. We'll join on 5=collA.y
                {$group: {_id: null, sum: {$sum: "$a"}}},
                {
                    $lookup: {
                        from: collA.getName(),
                        // Only collADoc1Match1.z has a match to `t` in collB, so we'll have one
                        // document after the $lookup-$unwind.
                        localField: "sum",
                        foreignField: "y",
                        as: "result",
                    },
                },
            ],
            [{_id: null, sum: 5, result: [collADoc1Match1, collADoc1Match2]}],
        );
    });
    it("Group on a field, then lookup-unwind on that field and add a new field based on the join result field", function () {
        assertPipelineResults(
            [
                // Similar to before, we have to group on a $$ROOT expression to avoid creating a
                // $project and preventing LU pushdown.
                {$group: {_id: null, sum: {$sum: "$a"}, docSizeSum: {$sum: {$bsonSize: "$$ROOT"}}}},
                {
                    $lookup: {
                        from: collA.getName(),
                        localField: "sum",
                        foreignField: "y",
                        as: "result",
                    },
                },
                {$unwind: "$result"},
                {$addFields: {resultZ: "$result.z"}},
            ],
            [
                {_id: null, sum: 5, docSizeSum: 102, result: collADoc1Match1, resultZ: 5},
                {_id: null, sum: 5, docSizeSum: 102, result: collADoc1Match2, resultZ: 11},
            ],
        );
    });
});
