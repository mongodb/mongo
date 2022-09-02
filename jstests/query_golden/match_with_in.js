/**
 * Test $match with $in is supported and returns correct results.
 */

(function() {
"use strict";

const coll = db.cqf_golden_match_with_in;
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
    {_id: 13, a: [null]},
];

jsTestLog('Inserting docs:');
show(docs);
assert.commandWorked(coll.insert(docs));
print(`Collection count: ${coll.count()}`);

const runTest = (filter) => {
    const pipeline = [{$match: filter}];
    jsTestLog(`Query: ${tojsononeline(pipeline)}`);
    show(coll.aggregate(pipeline));
};

const testFilters = [
    // Test comparison to null.
    {a: {$in: [null]}},

    // Test empty in-list.
    {a: {$in: []}},

    // Test traversal and type bracketing behavior.
    {a: {$in: [1]}},
    {a: {$in: ['1']}},
    {a: {$in: [1, '1']}},
    {a: {$in: [1, 2]}},
    {a: {$in: [1, 2, {}]}},

    // Test $in with $elemMatch.
    {a: {$elemMatch: {$in: [1, 2]}}},
    {a: {$elemMatch: {$in: [[1]]}}},
    {a: {$elemMatch: {$in: [[[1]]]}}},

    // Test comparisons to arrays.
    {a: {$in: [[]]}},
    {a: {$in: [[1]]}},
    {a: {$in: [[[1]]]}},

    // Test comparison to objects.
    {a: {$in: [{}, {c: 1}]}},

    // Test compound predicates.
    {a: {$in: [1, 2]}, 'b.c': {$in: [2, 3]}},
    {a: {$in: [1, 2]}, 'b.c': {$in: []}},
    {a: {$in: [1, 2]}, 'b.c': {$in: [null]}},

    // Test $type.
    {a: {$type: "array"}},
    {a: {$type: "double"}},
    {a: {$type: "object"}},
];

for (const filter of testFilters) {
    runTest(filter);
}
}());
