/**
 * Test out ElemMatchObjectMatchExpression
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const coll = db.jstests_elemmatch_object;
coll.drop();

assert.commandWorked(coll.insert([
    {a: {b: 1, c: 4}},
    {a: [{b: 1, c: 4}]},
    {a: [{b: 2, c: 3}]},
    {a: [{b: 1, c: 2}, {b: 3, c: 4}]},
    {a: [{b: 1, c: 2}, {b: 3, c: 4}, {b: 1, c: 4}]},
    {a: [{b: [], c: []}]},
    {a: [{b: [12, 2], c: [13, 3]}]},
    {a: [{b: [11, 1], c: [14, 4]}]},
    {a: [{b: {w: 1, x: 2}, c: {y: 3, z: 4}}]},
    {a: [{b: [{w: 5, x: 6}, {w: 1, x: 2}], c: [{y: 7, z: 8}, {y: 3, z: 4}]}]},
    {a: [{b: [{w: 5, x: 2}, {w: 1, x: 6}], c: [{y: 7, z: 4}, {y: 3, z: 8}]}]},
    {a: [{b: [{w: 5, x: 6}, {w: 1, x: 2}], c: [{y: 7, z: 4}, {y: 3, z: 8}]}]},
    {a: [{b: [{w: [11, 1], x: [12, 2]}], c: [{y: [13, 3], z: [14, 4]}]}]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {b: 2}}}, {_id: 0}).toArray(), [
    {a: [{b: 2, c: 3}]},
    {a: [{b: [12, 2], c: [13, 3]}]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {b: 1}}}, {_id: 0}).toArray(), [
    {a: [{b: 1, c: 4}]},
    {a: [{b: 1, c: 2}, {b: 3, c: 4}]},
    {a: [{b: 1, c: 2}, {b: 3, c: 4}, {b: 1, c: 4}]},
    {a: [{b: [11, 1], c: [14, 4]}]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {b: 1, c: 4}}}, {_id: 0}).toArray(), [
    {a: [{b: 1, c: 4}]},
    {a: [{b: 1, c: 2}, {b: 3, c: 4}, {b: 1, c: 4}]},
    {a: [{b: [11, 1], c: [14, 4]}]},
]));

assert(arrayEq(
    coll.find({a: {$elemMatch: {b: {$elemMatch: {}}, c: {$elemMatch: {}}}}}, {_id: 0}).toArray(), [
        {a: [{b: [{w: 5, x: 6}, {w: 1, x: 2}], c: [{y: 7, z: 8}, {y: 3, z: 4}]}]},
        {a: [{b: [{w: 5, x: 2}, {w: 1, x: 6}], c: [{y: 7, z: 4}, {y: 3, z: 8}]}]},
        {a: [{b: [{w: 5, x: 6}, {w: 1, x: 2}], c: [{y: 7, z: 4}, {y: 3, z: 8}]}]},
        {a: [{b: [{w: [11, 1], x: [12, 2]}], c: [{y: [13, 3], z: [14, 4]}]}]},
    ]));

assert(arrayEq(
    coll.find({a: {$elemMatch: {b: {$elemMatch: {w: 1, x: 2}}, c: {$elemMatch: {}}}}}, {_id: 0})
        .toArray(),
    [
        {a: [{b: [{w: 5, x: 6}, {w: 1, x: 2}], c: [{y: 7, z: 8}, {y: 3, z: 4}]}]},
        {a: [{b: [{w: 5, x: 6}, {w: 1, x: 2}], c: [{y: 7, z: 4}, {y: 3, z: 8}]}]},
        {a: [{b: [{w: [11, 1], x: [12, 2]}], c: [{y: [13, 3], z: [14, 4]}]}]},
    ]));

assert(arrayEq(
    coll.find({a: {$elemMatch: {b: {$elemMatch: {w: 1, x: 2}}, c: {$elemMatch: {y: 3, z: 4}}}}},
              {_id: 0})
        .toArray(),
    [
        {a: [{b: [{w: 5, x: 6}, {w: 1, x: 2}], c: [{y: 7, z: 8}, {y: 3, z: 4}]}]},
        {a: [{b: [{w: [11, 1], x: [12, 2]}], c: [{y: [13, 3], z: [14, 4]}]}]},
    ]));

assert(coll.drop());

assert.commandWorked(coll.insert([
    {},
    {a: 1},
    {a: "foo"},
    {a: []},
    {a: {}},
    {a: [1]},
    {a: ["foo"]},
    {a: [[]]},
    {a: [{}]},
    {a: [[1]]},
    {a: [1, []]},
    {a: [1, {}]},
    {a: [{b: 1}]},
]));

assert(arrayEq(coll.find({a: {$elemMatch: {}}}, {_id: 0}).toArray(), [
    {a: [[]]},
    {a: [{}]},
    {a: [[1]]},
    {a: [1, []]},
    {a: [1, {}]},
    {a: [{b: 1}]},
]));
})();
