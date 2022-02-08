/**
 * Test that $lookup behaves correctly for array key values.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/fixture_helpers.js");  // For isSharded.

const localColl = db.lookup_arrays_semantics_local;
const foreignColl = db.lookup_arrays_semantics_foreign;

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(foreignColl) && !isShardedLookupEnabled) {
    return;
}

// To make the tests easier to read, we run each with exactly one record in the foreign collection,
// so we don't need to check the content of the "matched" field but only that it's not empty.
function runTest(
    {testDescription, localData, localField, foreignRecord, foreignField, idsExpectedToMatch}) {
    localColl.drop();
    assert.commandWorked(localColl.insert(localData));

    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecord));

    const results = localColl.aggregate([{
        $lookup: {
            from: foreignColl.getName(),
            localField: localField,
            foreignField: foreignField,
            as: "matched"
        }
    }]).toArray();

    // Build the array of ids for the results that have non-empty array in the "matched" field.
    const matchedIds = results
                           .filter(function(x) {
                               return tojson(x.matched) != tojson([]);
                           })
                           .map(x => (x._id));

    // Order of the elements within the arrays is not significant for 'assertArrayEq'.
    assertArrayEq(
        {actual: matchedIds, expected: idsExpectedToMatch, extraErrorMsg: testDescription});
}

runTest({
    testDescription: "Scalar in local should match elements of arrays in foreign.",
    localData: [
        // Expect to match "a" to 1.
        {_id: 0, a: 1},
        {_id: 1, a: [1]},
        {_id: 2, a: [1, 2, 3]},
        {_id: 3, a: [1, [2, 3]]},
        {_id: 4, a: [1, [1, 2]]},
        {_id: 5, a: [1, 2, 1]},

        // Don't expect to match.
        {_id: 10, a: [[1]]},
        {_id: 11, a: [[1, 2], 3]},
        {_id: 12, a: 3},
        {_id: 13, a: [2, 3]},
        {_id: 14, a: {b: 1}}
    ],
    localField: "a",
    foreignRecord: {_id: 0, b: 1},
    foreignField: "b",
    idsExpectedToMatch: [0, 1, 2, 3, 4, 5]
});

runTest({
    testDescription: "Elements in an array in foreign should match to scalars in local.",
    localData: [
        // Expect to match "a" to [1, 2].
        {_id: 0, a: 1},
        {_id: 1, a: 2},
        {_id: 2, a: [1]},
        {_id: 3, a: [1, 3]},
        {_id: 4, a: [3, 2]},

        // Don't expect to match.
        {_id: 10, a: 3},
        {_id: 11, a: [[1]]},
        {_id: 12, a: [3, 4]},
    ],
    localField: "a",
    foreignRecord: {_id: 0, b: [1, 2]},
    foreignField: "b",
    idsExpectedToMatch: [0, 1, 2, 3, 4]
});

runTest({
    testDescription: "An array in foreign should match as a whole value.",
    localData: [
        // Expect to match "a" to [[1, 2], 3].
        {_id: 0, a: [[1, 2], 4]},
        {_id: 1, a: [[1, 2]]},

        // Don't expect to match.
        {_id: 10, a: [1, 2]},  // top-level arrays in local are always traversed
        {_id: 11, a: [[[1, 2]]]},
        {_id: 12, a: [[1, 2, 3]]},
    ],
    localField: "a",
    foreignRecord: {_id: 0, b: [[1, 2], 3]},
    foreignField: "b",
    idsExpectedToMatch: [0, 1]
});

runTest({
    testDescription: "Scalar in foreign should match dotted paths in local.",
    localData: [
        // Expect to match "a.x" to 1.
        {_id: 0, a: [{x: 1}, {x: 2}]},
        {_id: 1, a: [{x: [1, 2]}]},
        {_id: 2, a: {x: 1}},
        {_id: 3, a: {x: [1, 2]}},

        // Don't expect to match.
        {_id: 10, a: [{x: 2}, {x: 3}]},
        {_id: 11, a: 1},
        {_id: 12, a: [1]},
        {_id: 13, a: [[1]]},
        {_id: 14, a: [{y: 1}]},
    ],
    localField: "a.x",
    foreignRecord: {_id: 0, b: 1},
    foreignField: "b",
    idsExpectedToMatch: [0, 1, 2, 3]
});

runTest({
    testDescription: "Elements of array in foreign should match dotted paths in local.",
    localData: [
        // Expect to match "a.x" to [1, 2].
        {_id: 0, a: [{x: 1}, {x: 3}]},
        {_id: 1, a: [{x: [1, 3]}]},
        {_id: 2, a: {x: 1}},
        {_id: 3, a: {x: [1, 2]}},

        // Don't expect to match.
        {_id: 10, a: 1},
        {_id: 11, a: [1]},
        {_id: 12, a: [{x: 3}]},
        {_id: 13, a: [{y: 1}]},
    ],
    localField: "a.x",
    foreignRecord: {_id: 0, b: [1, 2]},
    foreignField: "b",
    idsExpectedToMatch: [0, 1, 2, 3]
});

runTest({
    testDescription: "An array in foreign should not match as a whole value to dotted paths.",
    localData: [
        // Expect to match "a.x" to [[1, 2], 3].

        // Don't expect to match.
        {_id: 10, a: [{x: 1}, {x: 2}]},
        {_id: 11, a: [{x: [1, 2]}]},
        {_id: 12, a: {x: [1, 2]}},
        {_id: 13, a: [1, 2]},
    ],
    localField: "a.x",
    foreignRecord: {_id: 0, b: [[1, 2], 3]},
    foreignField: "b",
    idsExpectedToMatch: []
});

runTest({
    testDescription: "Array of objects in foreign should match on subfield.",
    localData: [
        // Expect to match the same as if the foreign value was [1, 2].
        {_id: 0, a: 1},
        {_id: 1, a: 2},
        {_id: 2, a: [1]},
        {_id: 3, a: [1, 3]},
        {_id: 4, a: [3, 2]},
        {_id: 5, a: [[1, 2], 3]},

        // Don't expect to match.
        {_id: 10, a: 4},
        {_id: 11, a: [[1]]},
        {_id: 12, a: [3, 4]},
    ],
    localField: "a",
    foreignRecord: {_id: 0, b: [{x: 1}, {x: 2}]},
    foreignField: "b.x",
    idsExpectedToMatch: [0, 1, 2, 3, 4]
});

runTest({
    testDescription: "Empty array in foreign should only match to empty nested arrays.",
    localData: [
        // Expect to match
        {_id: 0, a: [[]]},
        {_id: 1, a: [[], 1]},

        // Don't expect to match.
        {_id: 10},
        {_id: 11, a: null},
        {_id: 12, a: []},
        {_id: 13, a: [null]},
        {_id: 14, a: [null, 1]},
        {_id: 15, a: 1},
        {_id: 16, a: [[[]]]},
        {_id: 17, a: [1, 2]},
    ],
    localField: "a",
    foreignRecord: {_id: 0, b: []},
    foreignField: "b",
    idsExpectedToMatch: [0, 1]
});

// This tests documents current behavior of the classic engine, which matches empty array in local
// to null in foreign. Update the test if SERVER-63368 is fixed.
runTest({
    testDescription: "Empty array in local and null in foreign.",
    localData: [
        // Expect to match "a" to null.
        {_id: 0, a: []},

        // Don't expect to match.
        {_id: 10, a: [[]]},
        {_id: 11, a: [[], 1]},
        {_id: 12, a: [1]},
    ],
    localField: "a",
    foreignRecord: {_id: 0, b: null},
    foreignField: "b",
    idsExpectedToMatch: [0]
});

// This tests documents current behavior of the classic engine, which matches empty array in local
// to missing in foreign. Update the test if SERVER-63368 is fixed.
runTest({
    testDescription: "Empty array in local and missing in foreign.",
    localData: [
        // Expect to match
        {_id: 0, a: []},

        // Don't expect to match.
        {_id: 10, a: [[]]},
        {_id: 11, a: [[], 1]},
        {_id: 12, a: [1]},
    ],
    localField: "a",
    foreignRecord: {_id: 0, no_b: true},
    foreignField: "b",
    idsExpectedToMatch: [0]
});

// This tests documents current behavior of the classic engine, which matches empty array in local
// to undefined in foreign. Update the test if SERVER-63368 is fixed.
runTest({
    testDescription: "Empty array in local and undefined in foreign.",
    localData: [
        // Expect to match
        {_id: 0, a: []},

        // Don't expect to match.
        {_id: 10, a: [[]]},
        {_id: 11, a: [[], 1]},
        {_id: 12, a: [1]},
    ],
    localField: "a",
    foreignRecord: {_id: 0, b: undefined},
    foreignField: "b",
    idsExpectedToMatch: [0]
});
}());
