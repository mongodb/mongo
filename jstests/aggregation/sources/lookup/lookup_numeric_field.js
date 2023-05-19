// Tests that numeric field components in $lookup and $graphLookup arguments behave correctly. This
// includes $lookup 'localField' and $graphLookup 'startsWith', 'connectFromField', and
// 'connectToField'.
// @tags: [
//   # Using a column scan removes the transformBy we search for.
//   assumes_no_implicit_index_creation,
// ]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getWinningPlan.

const local = db.local;
const foreign = db.foreign;

foreign.drop();
assert.commandWorked(foreign.insert({y: 3, z: 4}));

function testFieldTraversal(pipeline, localDoc, shouldMatchDoc, prefix) {
    local.drop();
    assert.commandWorked(local.insert(localDoc));

    // Test correctness.
    const results = db.local.aggregate(pipeline).toArray();
    if (shouldMatchDoc) {
        assert.eq(results, [{count: 1}]);
    } else {
        assert.eq(results.length, 0);
    }

    // Look for the transformBy.
    const explain = db.local.explain().aggregate(pipeline);
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

function testLookupLocalField(localField, localDoc, shouldMatchDoc, prefix) {
    // Some prefix of the localField argument gets pushed down to find as a "transformBy" since it's
    // the only field we need for this pipeline.
    // We should see
    // {transformBy: {prefix: true, _id: false}}
    const pipeline = [
        {$lookup: {from: "foreign", localField: localField, foreignField: "y", as: "docs"}},
        {$match: {"docs.0.z": 4}},
        {$count: "count"}
    ];
    testFieldTraversal(pipeline, localDoc, shouldMatchDoc, prefix);
}

function testGraphLookupStartsWith(localField, localDoc, shouldMatchDoc, prefix) {
    // Similar to the lookup transformBy case, but for $graphLookup.
    const pipeline = [
        {$graphLookup: {
            from: "foreign",
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

function testGraphLookupToFromField(foreignDocs, fromField, toField, expectedDocs) {
    foreign.drop();
    assert.commandWorked(foreign.insert(foreignDocs));

    const pipeline = [
        {$graphLookup: {
            from: "foreign",
            startWith: 0,
            connectFromField: fromField,
            connectToField: toField,
            as: "docs"
        }},
        {$project: {docs: {$sortArray: {input: "$docs", sortBy: {_id: 1}}}}}
    ];

    const result = local.aggregate(pipeline).toArray();
    assert.eq(result.length, 1);
    assert.eq(result[0].docs, expectedDocs);
}

// Test the $lookup 'localField' field.
{
    // Non-numeric cases shouldn't be affected.
    testLookupLocalField("a", {a: 3}, true, ["a"]);
    testLookupLocalField("a", {a: 1}, false, ["a"]);
    testLookupLocalField("a.b", {a: {b: 3}}, true, ["a", "b"]);
    testLookupLocalField("a.b.0", {a: {b: [3]}}, true, ["a", "b"]);

    // Basic numeric cases.
    testLookupLocalField("a.0", {a: [3, 2, 1]}, true, ["a"]);
    testLookupLocalField("a.0", {a: {"0": 3, "1": 2, "3": 1}}, true, ["a"]);
    testLookupLocalField("a.1", {a: [3, 2, 1]}, false, ["a"]);
    testLookupLocalField("a.3", {a: [3, 2, 1]}, false, ["a"]);
    testLookupLocalField("b.3", {a: [3, 2, 1]}, false, ["b"]);

    // Consecutive numeric fields.
    testLookupLocalField("c.1.0", {c: [0, [3, 4, 3], [1, 2]]}, true, ["c"]);
    testLookupLocalField("c.1.2", {c: [0, [3, 4, 3], [1, 2]]}, true, ["c"]);
    testLookupLocalField("c.0.0", {c: [0, [3, 4, 3], [1, 2]]}, false, ["c"]);
    testLookupLocalField("b.2.1", {a: [0, [3, 4, 3], [1, 2]]}, false, ["b"]);

    // Mix numeric and regular fields.
    testLookupLocalField("a.2.b.1", {a: [{}, {b: [2]}, {b: [1, 3]}]}, true, ["a"]);
    testLookupLocalField("a.2.b.1", {a: {"2": {b: [1, 3]}}}, true, ["a"]);
    testLookupLocalField("a.2.b.2", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a"]);
    testLookupLocalField("a.1.b.1", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a"]);
    testLookupLocalField("a.1.b.2", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a"]);

    // Test two regular fields then a numeric to make sure "transformBy" has "a.b" instead of just
    // "a".
    testLookupLocalField("a.b.0", {a: {b: [3]}}, true, ["a", "b"]);
    testLookupLocalField("a.b.c.1", {a: {b: {c: [1, 3]}}}, true, ["a", "b", "c"]);

    // Verify that $lookup does not treat 0-prefixed numeric fields as array indices.
    testLookupLocalField("a.00", {a: [3]}, false, ["a"]);
    testLookupLocalField("a.b.01", {a: {b: [1, 3]}}, false, ["a", "b"]);
    testLookupLocalField("a.00.b", {a: [{b: 3}]}, false, ["a"]);

    // Verify that $lookup always treats 0-prefixed numeric fields as field names.
    testLookupLocalField("a.00", {a: {"00": 3}}, true, ["a"]);
    testLookupLocalField("a.b.01", {a: {b: {"01": 3}}}, true, ["a", "b"]);
    testLookupLocalField("a.00.b", {a: {"00": {b: 3}}}, true, ["a"]);

    // Regular index fields shouldn't match "00"-type fields.
    testLookupLocalField("a.0", {a: {"00": 3}}, false, ["a"]);
    testLookupLocalField("a.b.1", {a: {b: {"01": 3}}}, false, ["a", "b"]);
    testLookupLocalField("a.0.b", {a: {"00": {b: 3}}}, false, ["a"]);
}

// Test the $graphLookup 'startsWith' field.
{
    // Non-numeric cases shouldn't be affected.
    testGraphLookupStartsWith("$a", {a: 3}, true, ["a"]);
    testGraphLookupStartsWith("$a", {a: 1}, false, ["a"]);
    testGraphLookupStartsWith("$a.b", {a: {b: 3}}, true, ["a", "b"]);
    testGraphLookupStartsWith("$a.b.0", {a: {b: {"0": 3}}}, true, ["a", "b", "0"]);
    testGraphLookupStartsWith("$a.b.0", {a: {b: [{"0": 3}]}}, true, ["a", "b", "0"]);
    testGraphLookupStartsWith("$a.b.0", {a: {b: [3]}}, false, ["a", "b", "0"]);
    testGraphLookupStartsWith("$a.0", {a: {"0": 3}}, true, ["a", "0"]);
    testGraphLookupStartsWith("$a.0", {a: {"0": 2}}, false, ["a", "0"]);
    testGraphLookupStartsWith("$a.0", {a: [3, 2, 1]}, false, ["a", "0"]);

    // Should traverse once.
    testGraphLookupStartsWith("$a.0", {a: [{"0": 3}]}, true, ["a", "0"]);
    testGraphLookupStartsWith("$a.0", {a: [[{"0": 3}]]}, false, ["a", "0"]);

    // Consecutive numeric fields.
    testGraphLookupStartsWith("$c.1.0", {c: {"1": {"0": 3}}}, true, ["c", "1", "0"]);
    testGraphLookupStartsWith("$c.1.0", {c: {"01": {"0": 3}}}, false, ["c", "1", "0"]);
    testGraphLookupStartsWith("$c.1.0", {c: {"1": {"00": 3}}}, false, ["c", "1", "0"]);
    testGraphLookupStartsWith("$c.1.0", {c: {"0": {"1": 3}}}, false, ["c", "1", "0"]);

    // Mix numeric and regular fields.
    testGraphLookupStartsWith("$a.2.b.1", {a: {"2": {b: {"1": 3}}}}, true, ["a", "2", "b", "1"]);
    testGraphLookupStartsWith(
        "$a.2.b.1", {a: [{}, {b: [2]}, {b: [1, 3]}]}, false, ["a", "2", "b", "1"]);

    testGraphLookupStartsWith("$a.00", {a: {"00": 3}}, true, ["a", "00"]);
    testGraphLookupStartsWith("$a.00", {a: [{"00": 3}]}, true, ["a", "00"]);
    testGraphLookupStartsWith("$a.00", {a: {"00": [3]}}, true, ["a", "00"]);
    testGraphLookupStartsWith("$a.00", {a: [{"00": [3]}]}, false, ["a", "00"]);
    testGraphLookupStartsWith("$a.00", {a: [3]}, false, ["a", "00"]);
}

local.drop();
foreign.drop();

assert.commandWorked(local.insert({_id: 0}));

// Test the $graphLookup 'connectFromField' field.
const fromSpecs = [
    // Finding a value of "1" should match the next document.
    {singleField: "0", doubleField: "00", array: [1, 2]},
    {singleField: "1", doubleField: "01", array: [2, 1]}
];
for (const spec of fromSpecs) {
    // "00"-type fields should act as field names.
    testGraphLookupToFromField([{_id: 1, to: 0, from: {[spec.doubleField]: 1}}, {_id: 2, to: 1}],
                               "from." + spec.doubleField,
                               "to",
                               [{_id: 1, to: 0, from: {[spec.doubleField]: 1}}, {_id: 2, to: 1}]);
    // "00"-type fields should not act as an index into an array.
    testGraphLookupToFromField([{_id: 1, to: 0, from: spec.array}, {_id: 2, to: 1}],
                               "from." + spec.doubleField,
                               "to",
                               [{_id: 1, to: 0, from: spec.array}]);
    // Regular numeric fields should not match "00"-type fields.
    testGraphLookupToFromField([{_id: 1, to: 0, from: {[spec.doubleField]: 1}}, {_id: 2, to: 1}],
                               "from." + spec.singleField,
                               "to",
                               [{_id: 1, to: 0, from: {[spec.doubleField]: 1}}]);
    // Regular numeric fields can act as an array index.
    testGraphLookupToFromField([{_id: 1, to: 0, from: spec.array}, {_id: 2, to: 1}],
                               "from." + spec.singleField,
                               "to",
                               [{_id: 1, to: 0, from: spec.array}, {_id: 2, to: 1}]);
    // "00"-type fields should not match "0"-type field names.
    testGraphLookupToFromField([{_id: 1, to: 0, from: {[spec.singleField]: 1}}, {_id: 2, to: 1}],
                               "from." + spec.doubleField,
                               "to",
                               [{_id: 1, to: 0, from: {[spec.singleField]: 1}}]);
    // Regular numeric fields can match themselves as field names.
    testGraphLookupToFromField([{_id: 1, to: 0, from: {[spec.singleField]: 1}}, {_id: 2, to: 1}],
                               "from." + spec.singleField,
                               "to",
                               [{_id: 1, to: 0, from: {[spec.singleField]: 1}}, {_id: 2, to: 1}]);
}

// Test the $graphLookup 'connectToField' field.
const toSpecs = [
    // Finding a value of "0" should match the document.
    {singleField: "0", doubleField: "00", array: [0, 2]},
    {singleField: "1", doubleField: "01", array: [2, 0]}
];
for (const spec of toSpecs) {
    // "00"-type fields should act as field names.
    testGraphLookupToFromField([{_id: 1, to: {[spec.doubleField]: 0}}],
                               "from",
                               "to." + spec.doubleField,
                               [{_id: 1, to: {[spec.doubleField]: 0}}]);
    // "00"-type fields should not act as an index into an array.
    testGraphLookupToFromField([{_id: 1, to: spec.array}], "from", "to." + spec.doubleField, []);
    // Regular numeric fields should not match "00"-type fields.
    testGraphLookupToFromField(
        [{_id: 1, to: {[spec.doubleField]: 0}}], "from", "to." + spec.singleField, []);
    // Regular numeric fields can act as an array index.
    testGraphLookupToFromField(
        [{_id: 1, to: spec.array}], "from", "to." + spec.singleField, [{_id: 1, to: spec.array}]);
    // "00"-type fields should not match "0"-type field names.
    testGraphLookupToFromField(
        [{_id: 1, to: {[spec.singleField]: 0}}], "from", "to." + spec.doubleField, []);
    // Regular numeric fields can match themselves as field names.
    testGraphLookupToFromField([{_id: 1, to: {[spec.singleField]: 0}}],
                               "from",
                               "to." + spec.singleField,
                               [{_id: 1, to: {[spec.singleField]: 0}}]);
}
}());
