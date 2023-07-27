/**
 * Test out ElemMatchValueMatchExpression
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const coll = db.jstests_elemmatch_value;
coll.drop();

assert.commandWorked(coll.insert([
    {a: 5},
    {a: [5]},
    {a: [3, 7]},
    {a: [[5]]},
    {a: [[3, 7]]},
    {a: [[[5]]]},
    {a: [[[3, 7]]]},
    {a: {b: 5}},
    {a: {b: [5]}},
    {a: {b: [3, 7]}},
    {a: [{b: 5}]},
    {a: [{b: 3}, {b: 7}]},
    {a: [{b: [5]}]},
    {a: [{b: [3, 7]}]},
    {a: [[{b: 5}]]},
    {a: [[{b: 3}, {b: 7}]]},
    {a: [[{b: [5]}]]},
    {a: [[{b: [3, 7]}]]}
]));

assert(arrayEq(coll.find({a: {$elemMatch: {$lt: 6, $gt: 4}}}, {_id: 0}).toArray(), [{a: [5]}]));

assert(arrayEq(coll.find({"a.b": {$elemMatch: {$lt: 6, $gt: 4}}}, {_id: 0}).toArray(), [
    {a: {b: [5]}},
    {a: [{b: [5]}]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {$elemMatch: {$lt: 6, $gt: 4}}}}, {_id: 0}).toArray(), [
    {a: [[5]]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {$type: "number"}}}, {_id: 0}).toArray(), [
    {a: [5]},
    {a: [3, 7]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {$type: "array"}}}, {_id: 0}).toArray(), [
    {a: [[5]]},
    {a: [[3, 7]]},
    {a: [[[5]]]},
    {a: [[[3, 7]]]},
    {a: [[{b: 5}]]},
    {a: [[{b: 3}, {b: 7}]]},
    {a: [[{b: [5]}]]},
    {a: [[{b: [3, 7]}]]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {b: {$lt: 6, $gt: 4}}}}, {_id: 0}).toArray(), [
    {a: [{b: 5}]},
    {a: [{b: [5]}]},
    {a: [{b: [3, 7]}]},
]));

assert(
    arrayEq(coll.find({a: {$elemMatch: {b: {$elemMatch: {$lt: 6, $gt: 4}}}}}, {_id: 0}).toArray(), [
        {a: [{b: [5]}]},
    ]));

assert(
    arrayEq(coll.find({a: {$elemMatch: {$elemMatch: {b: {$lt: 6, $gt: 4}}}}}, {_id: 0}).toArray(), [
        {a: [[{b: 5}]]},
        {a: [[{b: [5]}]]},
        {a: [[{b: [3, 7]}]]},
    ]));
})();
