/**
 * Tests the behavior of projection for dotted paths, including edge cases where the path only
 * sometimes exists.
 */
(function() {
"use strict";

const coll = db.projection_semantics;

// Tests that when 'projection' is applied to 'input', we get 'expectedOutput'.
// Tests that this remains true if indexes are added, or if we use aggregation instead of find.
function testInputOutput({input, projection, expectedOutput, interestingIndexes = []}) {
    coll.drop();
    assert.commandWorked(coll.insert(input));
    assert.docEq(coll.findOne({}, projection), expectedOutput);
    for (let indexSpec of interestingIndexes) {
        assert.commandWorked(coll.createIndex(indexSpec));
        assert.docEq(coll.find({}, projection).hint(indexSpec).toArray()[0], expectedOutput);
        assert.commandWorked(coll.dropIndex(indexSpec));
    }
    assert.docEq(coll.aggregate([{$project: projection}]).toArray()[0], expectedOutput);
}

// The basics: what happens when I include a top-level field?
(function testTopLevelInclusion() {
    // Test the basic "include a" projection. This should implicitly include the _id.
    const testIncludeA = (input, output) => testInputOutput({
        input: input,
        projection: {a: 1},
        expectedOutput: output,
        interestingIndexes: [{_id: 1, a: 1}, {a: 1, _id: 1}]
    });

    // Test some basic "normal" cases.
    testIncludeA({_id: 0, a: "demo", b: "other", x: "extra"}, {_id: 0, a: "demo"});
    testIncludeA({_id: 1, a: "demo", aWSuffix: "other", 'a.b': "extra"}, {_id: 1, a: "demo"});
    testIncludeA({_id: 2, a: null, b: "other", x: "extra"}, {_id: 2, a: null});

    // Test including "a" when "a" is missing/not present.
    // TODO SERVER-23229 this will return different results if there is a covering index, so here
    // but not elsewhere we don't use any "interestingIndexes".
    testInputOutput({
        input: {_id: 0, b: "other", x: "extra"},
        projection: {a: 1},
        expectedOutput: {_id: 0},
        interestingIndexes: []
    });
    testInputOutput(
        {input: {_id: 0}, projection: {a: 1}, expectedOutput: {_id: 0}, interestingIndexes: []});

    // Test a range of interesting values for "a". We expect everything to be preserved unmodified.
    const testIdentityInclusionA = (input) => testInputOutput({
        input: input,
        projection: {a: 1},
        expectedOutput: input,
        interestingIndexes: [{_id: 1, a: 1}, {a: 1, _id: 1}]
    });
    testIdentityInclusionA({_id: 1, a: null});
    testIdentityInclusionA({_id: 2, a: undefined});
    testIdentityInclusionA({_id: 3, a: {}});
    testIdentityInclusionA({_id: 4, a: []});
    testIdentityInclusionA({_id: 5, a: {x: 1, b: "scalar"}});
    testIdentityInclusionA({_id: 6, a: "scalar"});
    testIdentityInclusionA({_id: 7, a: {b: {}}});
    testIdentityInclusionA({_id: 8, a: [null]});
    testIdentityInclusionA({_id: 9, a: ["scalar"]});
    testIdentityInclusionA({_id: 10, a: [[]]});
    testIdentityInclusionA({_id: 11, a: [{}]});
    testIdentityInclusionA({_id: 12, a: [1, {}, 2]});
    testIdentityInclusionA({_id: 13, a: [[1, 2], [{}], 2]});
    testIdentityInclusionA({_id: 14, a: [{b: "scalar"}]});

    // Now test with the same documents but excluding the "_id" field.
    const testIncludeOnlyA = (input, output) => testInputOutput({
        input: input,
        projection: {a: 1, _id: 0},
        expectedOutput: output,
        // Can't use {a: "hashed"} here because it doesn't support array values.
        // Could use {a: "$**"}, but it can't be hinted without a query predicate so we'll leave
        // that coverage for elsewhere rather than add complexity to get a valid query predicate.
        interestingIndexes: [{a: 1}, {a: -1}]
    });

    // The "basics" again.
    testIncludeOnlyA({_id: 0, a: "demo", b: "other", x: "extra"}, {a: "demo"});
    testIncludeOnlyA({_id: 1, a: "demo", aWSuffix: "other", 'a.b': "extra"}, {a: "demo"});
    testIncludeOnlyA({_id: 2, a: null, b: "other", x: "extra"}, {a: null});

    // Missing 'a' value again.
    // TODO SERVER-23229 this will return different results if there is a covering index, so here
    // but not elsewhere we don't use any "interestingIndexes".
    testInputOutput({
        input: {_id: 0, b: "other", x: "extra"},
        projection: {a: 1, _id: 0},
        expectedOutput: {},
        interestingIndexes: []
    });
    testInputOutput(
        {input: {_id: 0}, projection: {a: 1, _id: 0}, expectedOutput: {}, interestingIndexes: []});

    // Just a couple of the cases above to confirm the same behavior just without the _id.
    testIncludeOnlyA({_id: 1, a: null}, {a: null});
    testIncludeOnlyA({_id: 3, a: {}}, {a: {}});
    testIncludeOnlyA({_id: 4, a: []}, {a: []});
    testIncludeOnlyA({_id: 5, a: {x: 1, b: "scalar"}}, {a: {x: 1, b: "scalar"}});
    testIncludeOnlyA({_id: 7, a: {b: {}}}, {a: {b: {}}});
    testIncludeOnlyA({_id: 14, a: [{b: "scalar"}]}, {a: [{b: "scalar"}]});
}());

// Now test one level of nesting - a single "dot" in the path.
(function testInclusionOneLevelOfNesting() {
    // Test the basic "include a.b" projection. This should implicitly include the _id.
    const testIncludeADotB = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b': 1},
        expectedOutput: output,
        interestingIndexes: [{_id: 1, 'a.b': 1}, {'a.b': 1, _id: 1}]
    });

    // Test some basic "normal" cases.
    // Test that it excludes extra fields at the root and at the sub-document.
    testIncludeADotB({_id: 0, a: {b: "demo", y: "extra"}, x: "extra"}, {_id: 0, a: {b: "demo"}});
    // Test that (at least for now) the dotted path doesn't work great here.
    testIncludeADotB({_id: 1, a: {b: "demo"}, 'a.b': "extra"}, {_id: 1, a: {b: "demo"}});
    // Test that '_id' within a sub-document is excluded.
    testIncludeADotB({_id: 2, a: {b: "demo", _id: "extra"}}, {_id: 2, a: {b: "demo"}});

    // Test array use case.
    testIncludeADotB({_id: 3, a: [{b: 1, c: 1}, {b: 2, c: 2}], x: "extra"},
                     {_id: 3, a: [{b: 1}, {b: 2}]});
    // Test that a missing field within an object in an array will show up as an empty object.
    testIncludeADotB({_id: 4, a: [{b: 1, c: 1}, {c: 2}], x: "extra"}, {_id: 4, a: [{b: 1}, {}]});

    // Test including "a.b" when "a.b" is missing/not present.
    //
    // TODO SERVER-23229 this will return different results if there is a covering index, so here
    // but not elsewhere we don't use any "interestingIndexes".
    // If there's a covered plan we will always construct {a: {b: null}} when we don't see an "a.b"
    // value, which is not always correct.
    //
    // Interestingly, this bug disappears if the index is multikey ('a' or 'a.b' are arrays), since
    // we will need to add a fetch to reconstruct the array anyway and will figure out the correct
    // answer in the process.
    const testADotBNoIndexes = (input, output) => testInputOutput(
        {input: input, projection: {'a.b': 1}, expectedOutput: output, interestingIndexes: []});

    testADotBNoIndexes({_id: 0, b: "other", x: "extra"}, {_id: 0});
    testADotBNoIndexes({_id: 1}, {_id: 1});
    testADotBNoIndexes({_id: 2, a: {}}, {_id: 2, a: {}});
    testADotBNoIndexes({_id: 3, a: "scalar"}, {_id: 3});

    const testIncludeOnlyADotB = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes: [{'a.b': 1}, {'a.b': -1}]
    });

    // The "basics" again - no _id this time.
    testIncludeOnlyADotB({_id: 0, a: {b: "demo", y: "extra"}, x: "extra"}, {a: {b: "demo"}});
    testIncludeOnlyADotB({_id: 1, a: {b: "demo"}, 'a.b': "extra"}, {a: {b: "demo"}});
    testIncludeOnlyADotB({_id: 2, a: {b: "demo", _id: "extra"}}, {a: {b: "demo"}});

    testIncludeOnlyADotB({_id: 3, a: [{b: 1, c: 1}, {b: 2, c: 2}], x: "extra"},
                         {a: [{b: 1}, {b: 2}]});
    testIncludeOnlyADotB({_id: 4, a: [{b: 1, c: 1}, {c: 2}], x: "extra"}, {a: [{b: 1}, {}]});

    // More cases where 'a.b' doesn't exist - but with arrays this time.
    testIncludeOnlyADotB({_id: 4, a: [], x: "extra"}, {a: []});
    testIncludeOnlyADotB({_id: 5, a: [{}, {}], x: "extra"}, {a: [{}, {}]});
    testIncludeOnlyADotB({_id: 6, a: ["scalar", "scalar"], x: "extra"}, {a: []});
    testIncludeOnlyADotB({_id: 7, a: [null]}, {a: []});
    // This is an interesting case: the scalars are ignored but the shadow documents are preserved.
    testIncludeOnlyADotB({_id: 8, a: ["scalar", {}, "scalar", {c: 1}], x: "extra"}, {a: [{}, {}]});
    // Further interest: the array within the array is preserved.
    testIncludeOnlyADotB({_id: 9, a: [[]]}, {a: [[]]});
    // But not the scalar elements of it.
    testIncludeOnlyADotB({_id: 10, a: [[1, 2, 3]]}, {a: [[]]});
    // But if there's a "b" again we see that.
    testIncludeOnlyADotB({_id: 10, a: [[1, {b: 1}, {b: 2, c: 2}, "scalar"]]},
                         {a: [[{b: 1}, {b: 2}]]});
    testIncludeOnlyADotB({
        _id: 10,
        a: [
            ["x", {b: 1}, {b: 2, c: 2}, "x"],
            [[{b: 1}]],
            [{b: 1}, [{b: 2}], [[{b: [2]}]]],
        ]
    },
                         {
                             a: [
                                 [{b: 1}, {b: 2}],
                                 [[{b: 1}]],
                                 [{b: 1}, [{b: 2}], [[{b: [2]}]]],
                             ]
                         });
    testIncludeOnlyADotB({_id: 11, a: [[], [[], [], [1], [{c: 1}]], {b: 1}]}, {
        a: [
            [],
            [[], [], [], [{}]],
            {b: 1},
        ]
    });

    // Test the case where two paths in a projection go through the same parent object.
    const testIncludeOnlyADotBAndADotC = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b': 1, 'a.c': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes:
            [{a: 1}, {'a.b': 1}, {'a.c': 1}, {'a.b': 1, 'a.c': 1}, {'a.b': 1, 'a.c': -1}]
    });
    testIncludeOnlyADotBAndADotC({_id: 0, a: {b: "scalar", c: "scalar", d: "extra"}},
                                 {a: {b: "scalar", c: "scalar"}});
    testIncludeOnlyADotBAndADotC({_id: 1, a: [{b: 1, c: 2, d: 3}, {b: 4, c: 5, d: 6}]},
                                 {a: [{b: 1, c: 2}, {b: 4, c: 5}]});

    // Array cases where one or both of the paths don't exist.
    testIncludeOnlyADotBAndADotC({_id: 5, a: [{b: 1, c: 2}, {b: 3, d: 4}]},
                                 {a: [{b: 1, c: 2}, {b: 3}]});
    testIncludeOnlyADotBAndADotC({_id: 6, a: [{c: 1, d: 2}, {b: 3, d: 4}]}, {a: [{c: 1}, {b: 3}]});
    testIncludeOnlyADotBAndADotC({_id: 7, a: []}, {a: []});
    testIncludeOnlyADotBAndADotC({_id: 8, a: [{b: 1, c: 2}, "extra", {b: 3, c: 4}]},
                                 {a: [{b: 1, c: 2}, {b: 3, c: 4}]});

    // Non-array cases where one or both of the paths don't exist.
    //
    // TODO SERVER-23229: This will return different results if there is a covering index, so here
    // but not elsewhere we don't use any "interestingIndexes" in test cases.
    const testIncludeADotBAndCNoIndexes = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b': 1, 'a.c': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes: []
    });

    testIncludeADotBAndCNoIndexes({_id: 2, a: {b: "scalar", d: "extra"}}, {a: {b: "scalar"}});
    testIncludeADotBAndCNoIndexes({_id: 3, a: {c: "scalar", d: "extra"}}, {a: {c: "scalar"}});
    testIncludeADotBAndCNoIndexes({_id: 4, a: {d: "extra"}}, {a: {}});
}());

(function testInclusionLevelsOfNesting() {
    // Test the basic "include a.b.c" projection. We've seen the implicit '_id' behavior above so
    // we'll always project _id out for these tests.
    const testIncludeADotBDotC = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b.c': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes: [{'a.b.c': 1}, {'a.b.c': -1}]
    });

    // Test some basic "normal" cases.
    // Test that it excludes extra fields at the root and at the sub-document
    testIncludeADotBDotC({a: {b: {c: "demo", z: "extra"}, y: "extra"}, x: "extra"},
                         {a: {b: {c: "demo"}}});

    // Test array use cases.
    // 'a' is an array.
    testIncludeADotBDotC({a: [{b: {c: 1, d: 1}}, {b: {c: 2, d: 2}}]},
                         {a: [{b: {c: 1}}, {b: {c: 2}}]});
    // 'a.b' is an array.
    testIncludeADotBDotC({a: {b: [{c: 1, d: 1}, {c: 2, d: 2}]}}, {a: {b: [{c: 1}, {c: 2}]}});
    // 'a' and 'a.b' are arrays.
    testIncludeADotBDotC(
        {a: [{b: [{c: 1, d: 1}, {c: 2, d: 2}]}, {b: [{c: 3, d: 3}, {c: 4, d: 4}]}]},
        {a: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {c: 4}]}]});
    // 'a' and 'a.b' and 'a.b.c' are arrays.
    testIncludeADotBDotC(
        {
            a: [
                {b: [{c: [1, 2, 3], d: 1}, {c: [2, 3, 4], d: 2}]},
                {b: [{c: [3, 4, 5], d: 3}, {c: [4, 5, 6], d: 4}]}
            ]
        },
        {a: [{b: [{c: [1, 2, 3]}, {c: [2, 3, 4]}]}, {b: [{c: [3, 4, 5]}, {c: [4, 5, 6]}]}]});

    // Test that when the path is missing we preserve the structure.
    testIncludeADotBDotC({
        a: [
            // No 'b' here.
            {bPrime: [{c: "x"}, {c: "x"}]},
            // No 'c' anywhere
            {b: [{cPrime: "x", d: 1}, {cPrime: "x", d: 2}]},
            // Only some 'c's
            {b: [{c: 3, d: 3}, {cPrime: "x", d: 4}, {c: 5}]},
            // One more without a 'b' to prove we get trailing empty objects.
            {bPrime: {somethingDifferent: "just because"}},
        ]
    },
                         {
                             a: [
                                 {/* bPrime was here */},
                                 {b: [{}, {} /* all cPrimes */]},
                                 {b: [{c: 3}, {/*cPrime was here */}, {c: 5}]},
                                 {/* bPrime again */}
                             ]
                         });

    // Test including "a.b.c" when "a.b.c" is missing/not present.
    //
    // TODO SERVER-23229 this will return different results if there is a covering index, so here
    // but not elsewhere we don't use any "interestingIndexes".
    // If there's a covered plan we will always construct {a: {b: {c: null}}} when we don't see an
    // "a.b.c" value, which is not always correct.
    //
    // Interestingly, this bug disappears if the index is multikey ('a.b.c' or any parent is an
    // array), since we will need to add a fetch to reconstruct the array anyway and will figure out
    // the correct answer in the process.
    const testADotBDotCNoIndexes = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b.c': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes: []
    });

    testADotBDotCNoIndexes({}, {});
    testADotBDotCNoIndexes({b: "other", x: "extra"}, {});
    testADotBDotCNoIndexes({a: "scalar"}, {});
    testADotBDotCNoIndexes({a: null}, {});
    testADotBDotCNoIndexes({a: {}}, {a: {}});
    testADotBDotCNoIndexes({a: {bPrime: {c: 1}}}, {a: {}});
    testADotBDotCNoIndexes({a: {b: "scalar"}}, {a: {}});
    testADotBDotCNoIndexes({a: {b: {x: 1, y: 1}}}, {a: {b: {}}});

    const testIncludeOnlyADotBDotC = (input, output) => testInputOutput({
        input: input,
        projection: {'a.b.c': 1, _id: 0},
        expectedOutput: output,
        interestingIndexes: [{'a.b.c': 1}, {'a.b.c': -1}]
    });

    // More cases where 'a.b.c' doesn't exist - but with arrays this time.

    // The cases from above with just 'a.b' are mostly the same for 'a.b.c':
    testIncludeOnlyADotBDotC({a: [], x: "extra"}, {a: []});
    testIncludeOnlyADotBDotC({a: [{}, {}], x: "extra"}, {a: [{}, {}]});
    testIncludeOnlyADotBDotC({a: ["scalar", "scalar"], x: "extra"}, {a: []});
    testIncludeOnlyADotBDotC({a: [null]}, {a: []});
    testIncludeOnlyADotBDotC({a: ["scalar", {}, "scalar", {c: 1}], x: "extra"}, {a: [{}, {}]});
    testIncludeOnlyADotBDotC({a: [[]]}, {a: [[]]});
    testIncludeOnlyADotBDotC({a: [[1, 2, 3]]}, {a: [[]]});
    // Now some with 'a.b' existing but no 'a.b.c'.
    testIncludeOnlyADotBDotC({a: {b: []}}, {a: {b: []}});
    testIncludeOnlyADotBDotC({a: {b: ["scalar"]}}, {a: {b: []}});
    testIncludeOnlyADotBDotC({a: {b: [[]]}}, {a: {b: [[]]}});
    testIncludeOnlyADotBDotC({a: [{b: "scalar"}]}, {a: [{}]});
    testIncludeOnlyADotBDotC({a: [{b: []}]}, {a: [{b: []}]});
    testIncludeOnlyADotBDotC({a: [{b: ["scalar"]}]}, {a: [{b: []}]});
    testIncludeOnlyADotBDotC({a: [{b: [[]]}]}, {a: [{b: [[]]}]});
    testIncludeOnlyADotBDotC({a: [{b: {x: 1}}]}, {a: [{b: {}}]});
    testIncludeOnlyADotBDotC({a: [{b: [{}]}]}, {a: [{b: [{}]}]});
    testIncludeOnlyADotBDotC({a: [[1, {b: 1}, {b: 2, c: 2}, "scalar"]]}, {a: [[{}, {}]]});
    testIncludeOnlyADotBDotC({a: [1, {b: [[1, 2], [{}], 2]}, 2]}, {a: [{b: [[], [{}]]}]});
    testIncludeOnlyADotBDotC({a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
                             {a: [[], [{b: [[], [{c: [[1, 2], [{}], 2]}]]}]]});
    testIncludeOnlyADotBDotC({
        a: [
            ["x", {b: 1}, {b: 2, c: 2}, "x"],
            [[{b: 1}]],
            [{b: 1}, [{b: 2}], [[{b: [2]}]]],
        ]
    },
                             {
                                 a: [
                                     [{}, {}],
                                     [[{}]],
                                     [{}, [{}], [[{b: []}]]],
                                 ]
                             });
    testIncludeOnlyADotBDotC({a: [[], [[], [], [1], [{x: 1}]], {b: 1}]}, {
        a: [
            [],
            [[], [], [], [{}]],
            {},
        ]
    });

    // Now some new cases where 'a.b.c' sometimes exists.
    // One array path.
    // 'a' is array.
    testIncludeOnlyADotBDotC({a: [{b: {x: 1, c: "scalar"}}]}, {a: [{b: {c: "scalar"}}]});
    testIncludeOnlyADotBDotC({a: [1, {b: {c: {x: 1}}}, 2]}, {a: [{b: {c: {x: 1}}}]});
    testIncludeOnlyADotBDotC({a: [1, {b: {x: 1, c: "scalar"}}, 2]}, {a: [{b: {c: "scalar"}}]});
    testIncludeOnlyADotBDotC({a: [[1, 2], [{b: {x: 1, c: "scalar"}}], 2]},
                             {a: [[], [{b: {c: "scalar"}}]]});
    // 'b' is array.
    testIncludeOnlyADotBDotC({a: {b: [{c: "scalar"}, {c: "scalar2"}]}},
                             {a: {b: [{c: "scalar"}, {c: "scalar2"}]}});
    testIncludeOnlyADotBDotC({a: {b: [1, {c: "scalar"}, 2]}}, {a: {b: [{c: "scalar"}]}});
    testIncludeOnlyADotBDotC({a: {b: [[1, 2], [{c: "scalar"}], 2]}},
                             {a: {b: [[], [{c: "scalar"}]]}});
    // 'c' is array.
    testIncludeOnlyADotBDotC({a: {x: 1, b: {x: 1, c: ["scalar"]}}}, {a: {b: {c: ["scalar"]}}});
    testIncludeOnlyADotBDotC({a: {b: {c: [[1, 2], [{}], 2]}}}, {a: {b: {c: [[1, 2], [{}], 2]}}});

    // Two arrays.
    // 'b' and 'c' are arrays.
    testIncludeOnlyADotBDotC({a: {x: 1, b: [1, {c: [[1, 2], [{}], 2]}, 2]}},
                             {a: {b: [{c: [[1, 2], [{}], 2]}]}});
    testIncludeOnlyADotBDotC({a: {x: 1, b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}},
                             {a: {b: [[], [{c: [[1, 2], [{}], 2]}]]}});
    // 'a' and 'b' are arrays.
    testIncludeOnlyADotBDotC({a: [{b: [{c: "scalar"}]}]}, {a: [{b: [{c: "scalar"}]}]});
    testIncludeOnlyADotBDotC({a: [1, {b: [{c: {x: 1}}]}, 2]}, {a: [{b: [{c: {x: 1}}]}]});
    testIncludeOnlyADotBDotC({a: [1, {b: [1, {c: "scalar"}, 2]}, 2]}, {a: [{b: [{c: "scalar"}]}]});
    testIncludeOnlyADotBDotC({a: [1, {b: [[1, 2], [{c: "scalar"}], 2]}, 2]},
                             {a: [{b: [[], [{c: "scalar"}]]}]});
    testIncludeOnlyADotBDotC({a: [[1, 2], [{b: [1, {c: "scalar"}, 2]}], 2]},
                             {a: [[], [{b: [{c: "scalar"}]}]]});
    testIncludeOnlyADotBDotC({a: [[1, 2], [{b: [[1, 2], [{c: "scalar"}], 2]}], 2]},
                             {a: [[], [{b: [[], [{c: "scalar"}]]}]]});
    // 'a' and 'c' are arrays.
    testIncludeOnlyADotBDotC({a: [1, {b: {c: [1, {}, 2]}}, 2]}, {a: [{b: {c: [1, {}, 2]}}]});
    testIncludeOnlyADotBDotC({a: [1, {b: {x: 1, c: [[]]}}, 2]}, {a: [{b: {c: [[]]}}]});
    testIncludeOnlyADotBDotC({a: [1, {b: {x: 1, c: [1, {}, 2]}}, 2]}, {a: [{b: {c: [1, {}, 2]}}]});

    // Three arrays.
    testIncludeOnlyADotBDotC({a: [{b: [{c: ["scalar"]}]}]}, {a: [{b: [{c: ["scalar"]}]}]});
    testIncludeOnlyADotBDotC({a: [{b: [1, {c: ["scalar"]}, 2]}]}, {a: [{b: [{c: ["scalar"]}]}]});
    testIncludeOnlyADotBDotC({a: [{b: [[1, 2], [{c: ["scalar"]}], 2]}]},
                             {a: [{b: [[], [{c: ["scalar"]}]]}]});
    testIncludeOnlyADotBDotC({a: [1, {b: [[1, 2], [{c: [1, {}, 2]}], 2]}, 2]},
                             {a: [{b: [[], [{c: [1, {}, 2]}]]}]});
    testIncludeOnlyADotBDotC({a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]},
                             {a: [[], [{b: [[], [{c: [[1, 2], [{}], 2]}]]}]]});
}());

// Now test the exclusion semantics. This part is a lot smaller since a lot of the behaviors mirror
// inclusion projection.
(function testExclusionSemantics() {
    // Test some basic top-level flat examples.
    testInputOutput({
        input: {_id: 0, a: 1, b: 1, c: 1},
        projection: {a: 0},
        expectedOutput: {_id: 0, b: 1, c: 1}
    });
    testInputOutput({
        input: {_id: 0, a: 1, b: 1, c: 1},
        projection: {a: 0, b: 0},
        expectedOutput: {_id: 0, c: 1}
    });

    // Test some dotted examples.
    testInputOutput({
        input: {_id: 0, a: {b: 1, c: 1, d: 1}, x: {y: 1, z: 1}},
        projection: {"a.b": 0},
        expectedOutput: {_id: 0, a: {c: 1, d: 1}, x: {y: 1, z: 1}}
    });
    // One notable difference between inclusion and exclusion projections is that parent's scalar
    // values remain untouched during an exclusion projection. The "scalar" here remains. In an
    // inclusion projection, these would disappear.
    testInputOutput({
        input: {_id: 0, a: ["scalar", {b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": 0},
        expectedOutput: {_id: 0, a: ["scalar", {c: 1, d: 1}, {c: 2}, {}]}
    });
    testInputOutput({
        input: {_id: 0, a: [{b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": 0, "a.c": 0},
        expectedOutput: {_id: 0, a: [{d: 1}, {}, {}]}
    });
    testInputOutput({
        input: {_id: 0, a: [{b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": 0, "a.c": 0},
        expectedOutput: {_id: 0, a: [{d: 1}, {}, {}]}
    });
    testInputOutput(
        {input: {_id: 0, a: []}, projection: {"a.b": 0}, expectedOutput: {_id: 0, a: []}});
    testInputOutput({
        input: {_id: 0, a: [[], [{b: [[1, 2], {c: 1, d: 1}]}]]},
        projection: {"a.b.c": 0},
        expectedOutput: {_id: 0, a: [[], [{b: [[1, 2], {d: 1}]}]]}
    });
}());

// Now test the semantics of projecting a computed field. Again this is fairly similar to inclusion
// projections but with a few interesting twists.
(function testComputedProjections() {
    // Test some basic top-level flat examples.
    testInputOutput({
        input: {_id: 0, a: 1, b: 1, c: 1},
        projection: {a: {$literal: 0}},
        expectedOutput: {_id: 0, a: 0}
    });
    testInputOutput({
        input: {_id: 0, a: 1, b: 1, c: 1},
        projection: {a: {$literal: 0}, b: "string"},
        expectedOutput: {_id: 0, a: 0, b: "string"}
    });

    // Test some dotted examples.
    testInputOutput({
        input: {_id: 0, a: {b: 1, c: 1, d: 1}, x: {y: 1, z: 1}},
        projection: {"a.b": "new value"},
        expectedOutput: {_id: 0, a: {b: "new value"}}
    });
    // One notable difference for computed projections is that they overwrite scalars rather than
    // ignoring or removing them.
    testInputOutput({
        input: {_id: 0, a: ["scalar", {b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": "new value"},
        expectedOutput:
            {_id: 0, a: [{b: "new value"}, {b: "new value"}, {b: "new value"}, {b: "new value"}]}
    });
    testInputOutput({
        input: {_id: 0, a: [{b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": "new value", "a.c": "new C"},
        expectedOutput: {
            _id: 0,
            a: [
                {b: "new value", c: "new C"},
                {b: "new value", c: "new C"},
                {b: "new value", c: "new C"}
            ]
        }
    });
    testInputOutput({
        input: {_id: 0, a: [{b: 1, c: 1, d: 1}, {b: 2, c: 2}, {b: 3}]},
        projection: {"a.b": "new value", "a.c": "new C"},
        expectedOutput: {
            _id: 0,
            a: [
                {b: "new value", c: "new C"},
                {b: "new value", c: "new C"},
                {b: "new value", c: "new C"}
            ]
        }
    });
    testInputOutput({
        input: {_id: 0, a: []},
        projection: {"a.b": "new value"},
        expectedOutput: {_id: 0, a: []}
    });
    // Computed projections will traverse through double arrays and preserve structure. For example,
    // we preserve two brackets inside the existing 'b: [[1,2]]' and leave the empty array in 'a'
    // untouched rather than replace it with {b: {c: "new value"}}.
    testInputOutput({
        input: {_id: 0, a: [[], [{b: [[1, 2], {c: 1, d: 1}]}]]},
        projection: {"a.b.c": "new value"},
        expectedOutput:
            {_id: 0, a: [[], [{b: [[{c: "new value"}, {c: "new value"}], {c: "new value"}]}]]}
    });
}());

// Test some miscellaneous properties of projections.
(function testMiscellaneousProjections() {
    // Test including and excluding _id only.
    testInputOutput({input: {_id: 0}, projection: {_id: 1}, expectedOutput: {_id: 0}});
    testInputOutput({input: {_id: 0}, projection: {_id: 0}, expectedOutput: {}});
    testInputOutput({
        input: {_id: 0, a: 1, b: 1},
        projection: {_id: 0},
        expectedOutput: {a: 1, b: 1},
    });

    // Test that you can specify nested paths with dots or as sub-objects and it'll mean the
    // same thing.
    testInputOutput({
        input: {measurements: {temperature: 20, pressure: 0.7, humidity: 0.4, time: new Date()}},
        projection: {'measurements.temperature': 1, 'measurements.pressure': 1, _id: 0},
        expectedOutput: {measurements: {temperature: 20, pressure: 0.7}},
        interestingIndexes: [{'measurements.temperature': 1, 'measurements.pressure': 1}]
    });
    testInputOutput({
        input: {measurements: {temperature: 20, pressure: 0.7, humidity: 0.4, time: new Date()}},
        projection: {measurements: {temperature: 1, pressure: 1}, _id: 0},
        expectedOutput: {measurements: {temperature: 20, pressure: 0.7}},
        interestingIndexes: [{'measurements.temperature': 1, 'measurements.pressure': 1}]
    });
    // Now with an exclusion projection.
    testInputOutput({
        input: {measurements: {temperature: 20, pressure: 0.7, humidity: 0.4, time: new Date()}},
        projection: {measurements: {humidity: 0, time: 0}, _id: 0},
        expectedOutput: {measurements: {temperature: 20, pressure: 0.7}},
    });
}());
}());
