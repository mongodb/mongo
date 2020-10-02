// Test $nor match expression.

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db.jstests_core_nor;
coll.drop();

function assertIds(query, expectedIds) {
    const resultIds = coll.find(query, {_id: 1}).map(d => d._id);
    assertArrayEq(expectedIds, resultIds);
}

assert.commandWorked(coll.insert([
    {_id: 1, a: 1},
    {_id: 2, a: 2},
    {_id: 3, a: 3},
]));

assertIds({$nor: [{a: 1}]}, [2, 3]);
assertIds({$nor: [{a: 1}, {a: 2}]}, [3]);
assertIds({$nor: [{a: 1}, {a: 2}, {a: 3}]}, []);

assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: 1, b: 2, c: 3},
    {_id: 2, a: 1, b: 2, c: 5},
    {_id: 3, a: 1, b: 5, c: 3},
    {_id: 4, a: 1, b: 5, c: 5},
    {_id: 5, a: 5, b: 2, c: 3},
    {_id: 6, a: 5, b: 2, c: 5},
    {_id: 7, a: 5, b: 5, c: 3},
    {_id: 8, a: 5, b: 5, c: 5},
]));

assertIds({$nor: [{a: 1}, {b: 2}, {c: 3}]}, [8]);
}());
