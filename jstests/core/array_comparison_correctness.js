/*
 * Demonstrate the expected behavior of $lt and $gt comparisons involving arrays. This is only
 * tested without an index, results between index and non-index behavior are compared in
 * array_index_and_nonIndex_consistent.js
 */

(function() {
    "use strict";
    load("jstests/aggregation/extras/utils.js");  // arrayEq
    const collName = jsTestName();
    const coll = db.getCollection(collName);
    coll.drop();
    const docsInColl = [
        {_id: 0, val: [1, 2]},
        {_id: 1, val: [3, 4]},
        {_id: 2, val: [3, 1]},
        {_id: 3, val: {"test": 5}},
        {_id: 4, val: [{"test": 7}]},
        {_id: 5, val: [true, false]},
        {_id: 6, val: 2},
        {_id: 7, val: 3},
        {_id: 8, val: 4},
        {_id: 9, val: [2]},
        {_id: 10, val: [3]},
        {_id: 11, val: [4]},
        {_id: 12, val: [1, true]},
        {_id: 13, val: [true, 1]},
        {_id: 14, val: [1, 4]},
        {_id: 15, val: [null]},
        {_id: 16, val: MinKey},
        {_id: 17, val: [MinKey]},
        {_id: 18, val: [MinKey, 3]},
        {_id: 19, val: [3, MinKey]},
        {_id: 20, val: MaxKey},
        {_id: 21, val: [MaxKey]},
        {_id: 22, val: [MaxKey, 3]},
        {_id: 23, val: [3, MaxKey]},
        {_id: 24, val: true},
        {_id: 25, val: false},
    ];
    assert.commandWorked(coll.insert(docsInColl));
    function generateFailedEqString(expected, found) {
        return "Expected: " + tojson(expected) + "\n Found: " + tojson(found);
    }
    function generateExpectedResults(indexes) {
        resultSet = [];
        indexes.forEach(function(index) {
            resultSet.push(docsInColl[index]);
        });
        return resultSet;
    }
    // Querying for LT/GT an integer returns all arrays that have a single integer element that
    // matches the query.
    let resultSet = coll.find({val: {$lt: 2}}).toArray();
    let expected = generateExpectedResults([0, 2, 12, 14]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$gt: 2}}).toArray();
    expected = generateExpectedResults([1, 2, 8, 14]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    // Test that querying for GT MinKey and LT MaxKey returns all results except for those values.
    resultSet = coll.find({val: {$gt: MinKey}}).toArray();
    let expectedInts = [...Array(25).keys()];
    expectedInts.splice(16, 1);
    expected = generateExpectedResults(expectedInts);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$lt: MaxKey}}).toArray();
    expectedInts = [...Array(25).keys()];
    expectedInts.splice(20, 1);
    expected = generateExpectedResults(expectedInts);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    // Test that querying for GT [MinKey] or LT [MaxKey] returns all other arrays, but nothing else.
    resultSet = coll.find({val: {$gt: [MinKey]}}).toArray();
    expected =
        generateExpectedResults([0, 1, 2, 4, 5, 9, 10, 11, 12, 13, 14, 15, 18, 19, 21, 22, 23]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$lt: [MaxKey]}}).toArray();
    expected =
        generateExpectedResults([0, 1, 2, 4, 5, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 22, 23]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    // Test that querying for LT/GT true returns no other types.
    resultSet = coll.find({val: {$gt: true}}).toArray();
    expected = [];
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$lt: true}}).toArray();
    expected = generateExpectedResults([25]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    // Test that querying for LT/GT arrays maintains lexicographic (version number) order and only
    // returns arrays.
    resultSet = coll.find({val: {$lt: [-1]}}).toArray();
    expected = generateExpectedResults([15, 17, 18]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$lt: [3]}}).toArray();
    expected = generateExpectedResults([0, 9, 12, 14, 15, 17, 18]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$lt: [3, 2]}}).toArray();
    expected = generateExpectedResults([0, 2, 9, 12, 14, 15, 17, 18]);
    assert(arrayEq(resultSet, expected), (resultSet, expected));

    resultSet = coll.find({val: {$gt: [2]}}).toArray();
    expected = generateExpectedResults([1, 2, 4, 5, 10, 11, 13, 19, 21, 22]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$gt: [15]}}).toArray();
    expected = generateExpectedResults([5, 13, 21, 22]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    resultSet = coll.find({val: {$gt: [3, 3]}}).toArray();
    expected = generateExpectedResults([1, 4, 5, 11, 13, 19, 21, 22]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    // $gt the empty array should return all arrays.
    resultSet = coll.find({val: {$gt: []}}).toArray();
    expected =
        generateExpectedResults([0, 1, 2, 4, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 21, 22, 23]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));

    // $lt the empty array should return no arrays.
    resultSet = coll.find({val: {$lt: []}}).toArray();
    expected = generateExpectedResults([3, 6, 7, 8, 16]);
    assert(arrayEq(resultSet, expected), generateFailedEqString(resultSet, expected));
})();
