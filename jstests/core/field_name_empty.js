/**
 * Test the behavior of match expressions with empty field names.
 * @tags: [
 *   assumes_read_concern_local,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db.field_name_empty;
coll.drop();

assert.commandWorked(coll.insertMany([
    {_id: 0, "": 1},
    {_id: 1, "": {"": 1}},
    {_id: 2, "": {"": {"": 1}}},
    {_id: 3, "": {"": {"": {"": 1}}}},
    {_id: 4, "": 1, a: 1}
]));

function runTest({filter, expected} = {}) {
    const result = coll.find(filter).toArray();
    assertArrayEq({actual: result, expected: expected});
}

runTest({filter: {".": 1}, expected: [{_id: 1, "": {"": 1}}]});
runTest({filter: {"..": 1}, expected: [{_id: 2, "": {"": {"": 1}}}]});
runTest({filter: {"...": 1}, expected: [{_id: 3, "": {"": {"": {"": 1}}}}]});
runTest({filter: {"": 1}, expected: [{_id: 0, "": 1}, {_id: 4, "": 1, a: 1}]});
runTest({filter: {"": 1, a: 1}, expected: [{_id: 4, "": 1, a: 1}]});
runTest({filter: {"": 1, a: 2}, expected: []});
})();
