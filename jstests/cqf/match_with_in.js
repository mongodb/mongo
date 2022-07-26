/**
 * Test $match with $in is supported and returns correct results.
 */

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load('jstests/libs/optimizer_utils.js');

(function() {
"use strict";

const coll = db.cqf_match_with_in;
coll.drop();

const docs = [
    {_id: 0},
    {_id: 1, a: null},
    {_id: 2, a: 1, b: {c: 1}},
    {_id: 3, a: 2, b: {c: 2}},
    {_id: 4, a: [], b: {c: 3}},
    {_id: 5, a: [1]},
    {_id: 6, a: ['1']},
    {_id: 7, a: [[1]]},
    {_id: 8, a: {}},
    {_id: 9, a: {c: 1}},
    {_id: 10, a: [{c: 1}]},
    {_id: 11, a: [[{c: 1}]]},
    {_id: 12, a: [[[1]]]},
];

assert.commandWorked(coll.insert(docs));

const runTest = (filter, expected) => {
    const result = coll.aggregate({$match: filter}).toArray();
    assertArrayEq({actual: result, expected: expected, extraErrorMsg: tojson({filter: filter})});

    // Sanity check that the query uses the bonsai optimizer.
    const explain = assert.commandWorked(db.runCommand(
        {explain: {aggregate: coll.getName(), pipeline: [{$match: filter}], cursor: {}}}));
    assert(usedBonsaiOptimizer(explain), tojson(explain));
};

const tests = [
    // Test comparison to null.
    {filter: {a: {$in: [null]}}, expected: [docs[0], docs[1]]},

    // Test empty in-list.
    {filter: {a: {$in: []}}, expected: []},

    // Test traversal and type bracketing behavior.
    {filter: {a: {$in: [1]}}, expected: [docs[2], docs[5]]},
    {filter: {a: {$in: ['1']}}, expected: [docs[6]]},
    {filter: {a: {$in: [1, '1']}}, expected: [docs[2], docs[5], docs[6]]},
    {filter: {a: {$in: [1, 2]}}, expected: [docs[2], docs[3], docs[5]]},
    {filter: {a: {$in: [1, 2, {}]}}, expected: [docs[2], docs[3], docs[5], docs[8]]},

    // Test $in with $elemMatch.
    {filter: {a: {$elemMatch: {$in: [1, 2]}}}, expected: [docs[5]]},
    {filter: {a: {$elemMatch: {$in: [[1]]}}}, expected: [docs[7]]},
    {filter: {a: {$elemMatch: {$in: [[[1]]]}}}, expected: [docs[12]]},

    // Test comparisons to arrays.
    {filter: {a: {$in: [[]]}}, expected: [docs[4]]},
    {filter: {a: {$in: [[1]]}}, expected: [docs[5], docs[7]]},
    {filter: {a: {$in: [[[1]]]}}, expected: [docs[7], docs[12]]},

    // Test comparison to objects.
    {filter: {a: {$in: [{}, {c: 1}]}}, expected: [docs[8], docs[9], docs[10]]},

    // Test compound predicates.
    {filter: {a: {$in: [1, 2]}, 'b.c': {$in: [2, 3]}}, expected: [docs[3]]},
    {filter: {a: {$in: [1, 2]}, 'b.c': {$in: []}}, expected: []},
    {filter: {a: {$in: [1, 2]}, 'b.c': {$in: [null]}}, expected: [docs[5]]},

    // Test $type.
    {
        filter: {a: {$type: "array"}},
        expected: [docs[4], docs[5], docs[6], docs[7], docs[10], docs[11], docs[12]]
    },
    {filter: {a: {$type: "double"}}, expected: [docs[2], docs[3], docs[5]]},
    {filter: {a: {$type: "object"}}, expected: [docs[8], docs[9], docs[10]]},
];

for (const testData of tests) {
    runTest(testData.filter, testData.expected);
}
}());
