// Tests that numeric field components in the $lookup localField and $graphLookup startsWith
// arguments are handled correctly.
// @tags: [
//   # Using a column scan removes the transformBy we search for.
//   assumes_no_implicit_index_creation,
// ]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getWinningPlan.

const outer = db.outer;
const inner = db.inner;

inner.drop();
assert.commandWorked(inner.insert({y: 3, z: 4}));

function testFieldTraversal(pipeline, localDoc, shouldMatchDoc, prefix) {
    outer.drop();
    assert.commandWorked(outer.insert(localDoc));

    // Test correctness.
    const results = db.outer.aggregate(pipeline).toArray();
    if (shouldMatchDoc) {
        assert.eq(results, [{count: 1}]);
    } else {
        assert.eq(results.length, 0);
    }

    // Look for the transformBy.
    const explain = db.outer.explain().aggregate(pipeline);
    const projStages = [
        ...getAggPlanStages(explain, "PROJECTION_SIMPLE"),
        ...getAggPlanStages(explain, "PROJECTION_DEFAULT")
    ];
    assert.gt(projStages.length, 0, explain);

    for (const projStage of projStages) {
        // We have the stage, now make sure we have the correct projection.
        let transform = projStage.transformBy;
        if (transform.hasOwnProperty(prefix.join("."))) {
            transform = transform[prefix.join(".")];
        } else {
            for (const field of prefix) {
                transform = transform[field];
            }
        }
        assert.eq(transform, true, explain);
    }
}

function testLookupFieldTraversal(localField, localDoc, shouldMatchDoc, prefix) {
    // Some prefix of the localField argument gets pushed down to find as a "transformBy" since it's
    // the only field we need for this pipeline.
    // We should see
    // {transformBy: {prefix: true, _id: false}}
    const pipeline = [
        {$lookup: {from: "inner", localField: localField, foreignField: "y", as: "docs"}},
        {$match: {"docs.0.z": 4}},
        {$count: "count"}
    ];
    testFieldTraversal(pipeline, localDoc, shouldMatchDoc, prefix);
}

function testGraphLookupFieldTraversal(localField, localDoc, shouldMatchDoc, prefix) {
    // Similar to the lookup transformBy case, but for $graphLookup.
    const pipeline = [
        {$graphLookup: {
            from: "inner",
            startWith: localField,
            connectFromField: "z",
            connectToField: "y",
            maxDepth: 0,
            as: "docs"
        }},
        {$match: {"docs.0.z": 4}},
        {$count: "count"}
    ];
    testFieldTraversal(pipeline, localDoc, shouldMatchDoc, prefix);
}

{
    // Non-numeric cases shouldn't be affected.
    testLookupFieldTraversal("a", {a: 3}, true, ["a"]);
    testLookupFieldTraversal("a", {a: 1}, false, ["a"]);
    testLookupFieldTraversal("a.b", {a: {b: 3}}, true, ["a", "b"]);
    testLookupFieldTraversal("a.b.0", {a: {b: [3]}}, true, ["a", "b"]);

    // Basic numeric cases.
    testLookupFieldTraversal("a.0", {a: [3, 2, 1]}, true, ["a"]);
    testLookupFieldTraversal("a.0", {a: {"0": 3, "1": 2, "3": 1}}, true, ["a"]);
    testLookupFieldTraversal("a.1", {a: [3, 2, 1]}, false, ["a"]);
    testLookupFieldTraversal("a.3", {a: [3, 2, 1]}, false, ["a"]);
    testLookupFieldTraversal("b.3", {a: [3, 2, 1]}, false, ["b"]);

    // Consecutive numeric fields.
    testLookupFieldTraversal("c.1.0", {c: [0, [3, 4, 3], [1, 2]]}, true, ["c"]);
    testLookupFieldTraversal("c.1.2", {c: [0, [3, 4, 3], [1, 2]]}, true, ["c"]);
    testLookupFieldTraversal("c.0.0", {c: [0, [3, 4, 3], [1, 2]]}, false, ["c"]);
    testLookupFieldTraversal("b.2.1", {a: [0, [3, 4, 3], [1, 2]]}, false, ["b"]);

    // Mix numeric and regular fields.
    testLookupFieldTraversal("a.2.b.1", {a: [{}, {b: [2]}, {b: [1, 3]}]}, true, ["a"]);
    testLookupFieldTraversal("a.2.b.1", {a: {"2": {b: [1, 3]}}}, true, ["a"]);
    testLookupFieldTraversal("a.2.b.2", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a"]);
    testLookupFieldTraversal("a.1.b.1", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a"]);
    testLookupFieldTraversal("a.1.b.2", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a"]);

    // Test two regular fields then a numeric to make sure "transformBy" has "a.b" instead of just
    // "a".
    testLookupFieldTraversal("a.b.0", {a: {b: [3]}}, true, ["a", "b"]);
    testLookupFieldTraversal("a.b.c.1", {a: {b: {c: [1, 3]}}}, true, ["a", "b", "c"]);

    // TODO after SERVER-76470 this should be able to be removed.
    if (assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
            .internalQueryFrameworkControl == "forceClassicEngine") {
        // These are cases where "00"-type fields are treated as an index, and SBE does not behave
        // this way.
        testLookupFieldTraversal("a.00", {a: [3]}, true, ["a"]);
        testLookupFieldTraversal("a.b.01", {a: {b: [1, 3]}}, true, ["a", "b"]);
        testLookupFieldTraversal("a.00.b", {a: [{b: 3}]}, true, ["a"]);
    }

    testLookupFieldTraversal("a.00", {a: {"00": 3}}, true, ["a"]);
    testLookupFieldTraversal("a.b.01", {a: {b: {"01": 3}}}, true, ["a", "b"]);
    testLookupFieldTraversal("a.00.b", {a: {"00": {b: 3}}}, true, ["a"]);
}

{
    // Non-numeric cases shouldn't be affected.
    testGraphLookupFieldTraversal("$a", {a: 3}, true, ["a"]);
    testGraphLookupFieldTraversal("$a", {a: 1}, false, ["a"]);
    testGraphLookupFieldTraversal("$a.b", {a: {b: 3}}, true, ["a", "b"]);
    testGraphLookupFieldTraversal("$a.b.0", {a: {b: {"0": 3}}}, true, ["a", "b", "0"]);
    testGraphLookupFieldTraversal("$a.b.0", {a: {b: [{"0": 3}]}}, true, ["a", "b", "0"]);
    testGraphLookupFieldTraversal("$a.b.0", {a: {b: [3]}}, false, ["a", "b", "0"]);
    testGraphLookupFieldTraversal("$a.0", {a: {"0": 3}}, true, ["a", "0"]);
    testGraphLookupFieldTraversal("$a.0", {a: {"0": 2}}, false, ["a", "0"]);
    testGraphLookupFieldTraversal("$a.0", {a: [3, 2, 1]}, false, ["a", "0"]);

    // Should traverse once.
    testGraphLookupFieldTraversal("$a.0", {a: [{"0": 3}]}, true, ["a", "0"]);
    testGraphLookupFieldTraversal("$a.0", {a: [[{"0": 3}]]}, false, ["a", "0"]);

    // Consecutive numeric fields.
    testGraphLookupFieldTraversal("$c.1.0", {c: {"1": {"0": 3}}}, true, ["c", "1", "0"]);
    testGraphLookupFieldTraversal("$c.1.0", {c: {"01": {"0": 3}}}, false, ["c", "1", "0"]);
    testGraphLookupFieldTraversal("$c.1.0", {c: {"1": {"00": 3}}}, false, ["c", "1", "0"]);
    testGraphLookupFieldTraversal("$c.1.0", {c: {"0": {"1": 3}}}, false, ["c", "1", "0"]);

    // Mix numeric and regular fields.
    testGraphLookupFieldTraversal(
        "$a.2.b.1", {a: {"2": {b: {"1": 3}}}}, true, ["a", "2", "b", "1"]);
    testGraphLookupFieldTraversal(
        "$a.2.b.1", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a", "2", "b", "1"]);

    testGraphLookupFieldTraversal("$a.00", {a: {"00": 3}}, true, ["a", "00"]);
    testGraphLookupFieldTraversal("$a.00", {a: [{"00": 3}]}, true, ["a", "00"]);
    testGraphLookupFieldTraversal("$a.00", {a: {"00": [3]}}, true, ["a", "00"]);
    testGraphLookupFieldTraversal("$a.00", {a: [{"00": [3]}]}, false, ["a", "00"]);
    testGraphLookupFieldTraversal("$a.00", {a: [3]}, false, ["a", "00"]);
}
}());
