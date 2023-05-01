/**
 * Testing simple inclusion/exclusion projections with dotted path fields while forcing CQF.
 * Many of these tests are similar/repeats of core/projection_semantics.js
 */

(function() {
"use strict";
load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load('jstests/libs/optimizer_utils.js');

const coll = db.cqf_project;

// Tests that when 'projection' is applied to 'input', we get 'expectedOutput'.
// Tests that this remains true if indexes are added.
function testInputOutput({input, projection, expectedOutput, interestingIndexes = []}) {
    coll.drop();
    assert.commandWorked(coll.insert(input));
    const result = coll.aggregate([{$project: projection}]).toArray();
    assertArrayEq(
        {actual: result, expected: expectedOutput, extraErrorMsg: tojson({project: projection})});

    for (let indexSpec of interestingIndexes) {
        assert.commandWorked(coll.createIndex(indexSpec));
        const result = coll.aggregate([{$project: projection}]).toArray();
        assertArrayEq({
            actual: result,
            expected: expectedOutput,
            extraErrorMsg: tojson({project: projection})
        });
        assert.commandWorked(coll.dropIndex(indexSpec));
    }

    const explain = assert.commandWorked(db.runCommand(
        {explain: {aggregate: coll.getName(), pipeline: [{$project: projection}], cursor: {}}}));
    assert(usedBonsaiOptimizer(explain), tojson(explain));
}

(function testInclusionOneLevelOfNesting() {
    // Test the case where two paths in a projection go through the same parent object.
    const testIncludeOnlyADotBAndADotC = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b': 1, 'a.c': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes: [{'a.b': 1, 'a.c': 1}, {'a.b': -1}]
    });
    testIncludeOnlyADotBAndADotC({_id: 0, a: {b: "scalar", c: "scalar", d: "extra"}},
                                 [{a: {b: "scalar", c: "scalar"}}]);
    testIncludeOnlyADotBAndADotC({_id: 1, a: [{b: 1, c: 2, d: 3}, {b: 4, c: 5, d: 6}]},
                                 [{a: [{b: 1, c: 2}, {b: 4, c: 5}]}]);

    // Array cases where one or both of the paths don't exist.
    testIncludeOnlyADotBAndADotC({_id: 5, a: [{b: 1, c: 2}, {b: 3, d: 4}]},
                                 [{a: [{b: 1, c: 2}, {b: 3}]}]);
    testIncludeOnlyADotBAndADotC({_id: 6, a: [{c: 1, d: 2}, {b: 3, d: 4}]},
                                 [{a: [{c: 1}, {b: 3}]}]);
    testIncludeOnlyADotBAndADotC({_id: 7, a: []}, [{a: []}]);
    testIncludeOnlyADotBAndADotC({_id: 8, a: [{b: 1, c: 2}, "extra", {b: 3, c: 4}]},
                                 [{a: [{b: 1, c: 2}, {b: 3, c: 4}]}]);
}());

(function testInclusionNestedId() {
    // Test the case where two paths in a projection go through the same parent object.
    const testIncludeOnlyADotBAndId = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b': 1, _id: 1},
        expectedOutput: output,
        interestingIndexes: []
    });
    testIncludeOnlyADotBAndId({_id: {a: 0}, a: {b: "scalar", c: "scalar", d: "extra"}},
                              [{_id: {a: 0}, a: {b: "scalar"}}]);
    testIncludeOnlyADotBAndId({_id: {a: 1}, a: [{b: 1, c: 2, d: 3}, {b: 4, c: 5, d: 6}]},
                              [{_id: {a: 1}, a: [{b: 1}, {b: 4}]}]);

    // Array cases where one or both of the paths don't exist.
    testIncludeOnlyADotBAndId({_id: {a: 5}, a: [{b: 1, c: 2}, {b: 3, d: 4}]},
                              [{_id: {a: 5}, a: [{b: 1}, {b: 3}]}]);
    testIncludeOnlyADotBAndId({_id: {a: 6}, a: [{c: 1, d: 2}, {b: 3, d: 4}]},
                              [{_id: {a: 6}, a: [{}, {b: 3}]}]);
    testIncludeOnlyADotBAndId({_id: {a: 7}, a: []}, [{_id: {a: 7}, a: []}]);
    testIncludeOnlyADotBAndId({_id: {a: 8}, a: [{b: 1}, "extra", {b: 3}]},
                              [{_id: {a: 8}, a: [{b: 1}, {b: 3}]}]);
}());

(function testInclusionLevelsOfNesting() {
    const testIncludeADotBDotC = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b.c': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes: [{'a.b.c': 1}, {'a.b': 1}]
    });
    // The cases from above with just 'a.b' are mostly the same for 'a.b.c':
    testIncludeADotBDotC({a: [], x: "extra"}, [{a: []}]);
    testIncludeADotBDotC({a: [{}, {}], x: "extra"}, [{a: [{}, {}]}]);
    testIncludeADotBDotC({a: ["scalar", "scalar"], x: "extra"}, [{a: []}]);
    testIncludeADotBDotC({a: [null]}, [{a: []}]);
    testIncludeADotBDotC({a: ["scalar", {}, "scalar", {c: 1}], x: "extra"}, [{a: [{}, {}]}]);
    testIncludeADotBDotC({a: [[]]}, [{a: [[]]}]);
    testIncludeADotBDotC({a: [[1, 2, 3]]}, [{a: [[]]}]);
    // Now some with 'a.b' existing but no 'a.b.c'.
    testIncludeADotBDotC({a: {b: []}}, [{a: {b: []}}]);
    testIncludeADotBDotC({a: {b: ["scalar"]}}, [{a: {b: []}}]);
    testIncludeADotBDotC({a: {b: [[]]}}, [{a: {b: [[]]}}]);
    testIncludeADotBDotC({a: [{b: "scalar"}]}, [{a: [{}]}]);
    testIncludeADotBDotC({a: [{b: []}]}, [{a: [{b: []}]}]);
    testIncludeADotBDotC({a: [{b: ["scalar"]}]}, [{a: [{b: []}]}]);
    testIncludeADotBDotC({a: [{b: [[]]}]}, [{a: [{b: [[]]}]}]);
    testIncludeADotBDotC({a: [{b: {x: 1}}]}, [{a: [{b: {}}]}]);
    testIncludeADotBDotC({a: [{b: [{}]}]}, [{a: [{b: [{}]}]}]);
    testIncludeADotBDotC({a: [[1, {b: 1}, {b: 2, c: 2}, "scalar"]]}, [{a: [[{}, {}]]}]);
    testIncludeADotBDotC({a: [1, {b: [[1, 2], [{}], 2]}, 2]}, [{a: [{b: [[], [{}]]}]}]);
    testIncludeADotBDotC({a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
                         [{a: [[], [{b: [[], [{c: [[1, 2], [{}], 2]}]]}]]}]);
    testIncludeADotBDotC({a: [{b: [{c: ["scalar"]}]}]}, [{a: [{b: [{c: ["scalar"]}]}]}]);
    testIncludeADotBDotC({a: [{b: [1, {c: ["scalar"]}, 2]}]}, [{a: [{b: [{c: ["scalar"]}]}]}]);
    testIncludeADotBDotC({a: [{b: [[1, 2], [{c: ["scalar"]}], 2]}]},
                         [{a: [{b: [[], [{c: ["scalar"]}]]}]}]);
    testIncludeADotBDotC({a: [1, {b: [[1, 2], [{c: [1, {}, 2]}], 2]}, 2]},
                         [{a: [{b: [[], [{c: [1, {}, 2]}]]}]}]);
    testIncludeADotBDotC({a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
                         [{a: [[], [{b: [[], [{c: [[1, 2], [{}], 2]}]]}]]}]);
}());

(function testExclusionSemantics() {
    const testInputOutputExclusion = ({input, projection, expectedOutput}) => testInputOutput({
        input: input,
        projection: projection,
        expectedOutput: expectedOutput,
    });

    testInputOutputExclusion({
        input: {_id: 9, a: {b: 1, c: 1, d: 1}, x: {y: 1, z: 1}},
        projection: {"a.b": 0},
        expectedOutput: [{_id: 9, a: {c: 1, d: 1}, x: {y: 1, z: 1}}]
    });
    testInputOutputExclusion({
        input: {_id: 10, a: ["scalar", {b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": 0},
        expectedOutput: [{_id: 10, a: ["scalar", {c: 1, d: 1}, {c: 2}, {}]}]
    });
    testInputOutputExclusion({
        input: {_id: 11, a: [{b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": 0, "a.c": 0},
        expectedOutput: [{_id: 11, a: [{d: 1}, {}, {}]}]
    });
    testInputOutputExclusion({
        input: {_id: 11, a: [[], [{b: [[1, 2], {c: 1, d: 1}]}]]},
        projection: {"a.b.c": 0},
        expectedOutput: [{_id: 11, a: [[], [{b: [[1, 2], {d: 1}]}]]}]
    });
}());

// Simple exclusion and inclusions over multiple documents.
(function testSimple() {
    const docs = [
        {_id: 0, a: 1, b: {c: 1, d: 1}},
        {_id: 1, a: 2, b: {c: 2, d: 2}},
        {_id: 2, a: [], b: {c: 3, d: 3}},
    ];
    testInputOutput({
        input: docs,
        projection: {a: 1},
        expectedOutput: [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: []}],
        interestingIndexes: [],
    });
    testInputOutput({
        input: docs,
        projection: {a: 1, _id: 0},
        expectedOutput: [{a: 1}, {a: 2}, {a: []}],
        interestingIndexes: [],
    });
    testInputOutput({
        input: docs,
        projection: {"b.c": 1, _id: 0},
        expectedOutput: [{b: {c: 1}}, {b: {c: 2}}, {b: {c: 3}}],
        interestingIndexes: [],
    });
    testInputOutput({
        input: docs,
        projection: {a: 0, _id: 0},
        expectedOutput: [{b: {c: 1, d: 1}}, {b: {c: 2, d: 2}}, {b: {c: 3, d: 3}}],
        interestingIndexes: [],
    });
    testInputOutput({
        input: docs,
        projection: {a: 0, _id: 1},
        expectedOutput:
            [{_id: 0, b: {c: 1, d: 1}}, {_id: 1, b: {c: 2, d: 2}}, {_id: 2, b: {c: 3, d: 3}}],
        interestingIndexes: [],
    });
}());
}());
