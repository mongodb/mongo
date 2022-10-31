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
    {_id: 4, "": 1, a: 1},
    {_id: 5, x: 3},
    {_id: 6, x: [3]},
    {_id: 7, x: {"": 3}},
    {_id: 8, x: {"": [3]}},
    {_id: 9, x: [{"": 3}]},
    {_id: 10, x: [{"": [3]}]},
    {_id: 11, x: {"": [{"": 3}]}},
    {_id: 12, x: {"": {y: 3}}},
    {_id: 13, "": [1]},
]));

function runTest({filter, expected} = {}) {
    const result = coll.find(filter).toArray();
    const explain = coll.explain("executionStats").find(filter).finish();
    assertArrayEq({actual: result, expected: expected, extraErrorMsg: tojson(explain)});
}

runTest({filter: {".": 1}, expected: [{_id: 1, "": {"": 1}}, {_id: 13, "": [1]}]});
runTest({filter: {"..": 1}, expected: [{_id: 2, "": {"": {"": 1}}}]});
runTest({filter: {"...": 1}, expected: [{_id: 3, "": {"": {"": {"": 1}}}}]});
runTest({filter: {"": 1}, expected: [{_id: 0, "": 1}, {_id: 4, "": 1, a: 1}, {_id: 13, "": [1]}]});
runTest({filter: {"": 1, a: 1}, expected: [{_id: 4, "": 1, a: 1}]});
runTest({filter: {"": 1, a: 2}, expected: []});
runTest({
    filter: {'x.': 3},
    expected: [{_id: 6, x: [3]}, {_id: 7, x: {"": 3}}, {_id: 8, x: {"": [3]}}]
});
runTest({filter: {'x..y': 3}, expected: [{_id: 12, x: {"": {y: 3}}}]});
runTest({filter: {'x..': 3}, expected: [{_id: 8, x: {"": [3]}}, {_id: 10, x: [{"": [3]}]}]});
runTest({
    filter: {$and: [{'x.': 3}, {_id: {$lt: 8}}]},
    expected: [{_id: 6, x: [3]}, {_id: 7, x: {"": 3}}]
});
runTest({
    filter: {"x.": {$exists: false}},
    expected: [
        {_id: 0, "": 1},
        {_id: 1, "": {"": 1}},
        {_id: 2, "": {"": {"": 1}}},
        {_id: 3, "": {"": {"": {"": 1}}}},
        {_id: 4, "": 1, a: 1},
        {_id: 5, x: 3},
        {_id: 13, "": [1]},
    ]
});
runTest({
    filter: {x: {$elemMatch: {"": 3}}},
    expected: [{_id: 9, x: [{"": 3}]}, {_id: 10, x: [{"": [3]}]}]
});
runTest({filter: {x: {$elemMatch: {"": {$type: "array"}}}}, expected: [{_id: 10, x: [{"": [3]}]}]});
runTest({
    filter: {"x.": {$elemMatch: {"": 3}}},
    expected: [{_id: 9, x: [{"": 3}]}, {_id: 10, x: [{"": [3]}]}, {_id: 11, x: {"": [{"": 3}]}}]
});
})();
