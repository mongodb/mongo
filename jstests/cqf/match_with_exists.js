/**
 * Test $match with $exists is supported and returns correct results.
 */

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.

(function() {
"use strict";

const coll = db.cqf_match_with_exists;

const runTest = (filter, expected) => {
    const result = coll.aggregate({$match: filter}).toArray();
    assertArrayEq({actual: result, expected: expected, extraErrorMsg: tojson({filter: filter})});
};

const runWithData = (docs, tests) => {
    coll.drop();
    assert.commandWorked(coll.insert(docs));
    for (const testData of tests(docs)) {
        runTest(testData.filter, testData.expected);
    }
};

runWithData(
    [
        {_id: 0},
        {_id: 1, a: null},
        {_id: 2, a: 1},
        {_id: 3, b: null},
        {_id: 4, b: 2},
        {_id: 5, 'a': {'b': 3}},
        {_id: 6, 'a': [{'b': 4}]}
    ],
    filters =>
        [{filter: {a: {$exists: true}}, expected: [filters[1], filters[2], filters[5], filters[6]]},
         {
             filter: {a: {$not: {$exists: false}}},
             expected: [filters[1], filters[2], filters[5], filters[6]]
         },
         {filter: {a: {$exists: false}}, expected: [filters[0], filters[3], filters[4]]},
         {filter: {b: {$exists: true}}, expected: [filters[3], filters[4]]},
         {filter: {'a.b': {$exists: true}}, expected: [filters[5], filters[6]]},
         {
             filter: {'a.b': {$exists: false}},
             expected: [filters[0], filters[1], filters[2], filters[3], filters[4]]
         },
]);

runWithData(
    [
        {_id: 1, a: []},
    ],
    filters => [{filter: {'a': {$exists: true}}, expected: [filters[0]]},
                {filter: {'a': {$exists: false}}, expected: []}]);

runWithData(
    [
        {_id: 1, a: false},
    ],
    filters => [{filter: {'a': {$exists: true}}, expected: [filters[0]]},
                {filter: {'a': {$exists: false}}, expected: []}]);

runWithData([{_id: 1, a: [{'b': 2}, {'a': 1}]}],
            filters => [{filter: {'a.a': {$exists: true}}, expected: [filters[0]]},
                        {filter: {'a.a': {$exists: false}}, expected: []},
                        {filter: {'a.b': {$exists: true}}, expected: [filters[0]]}]);

runWithData([{_id: 1, a: [[{b: 1}]]}],
            filters => [{filter: {'a.b': {$exists: false}}, expected: [filters[0]]},
                        {filter: {'a.b': {$exists: true}}, expected: []}]);

runWithData(
    [
        {_id: 1, a: [1]},
        {_id: 2, a: [2]},
    ],
    filters => [{filter: {'a': {$elemMatch: {$exists: true}}}, expected: [filters[0], filters[1]]},
                {filter: {'a': {$elemMatch: {$exists: false}}}, expected: []}]);
})();
