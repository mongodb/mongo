(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/optimizer_utils.js");      // For assertValueOnPlanPath.

const c = db.cqf_not;
c.drop();

assert.commandWorked(c.insertMany([
    {a: 1},
    {a: 2},
    {a: ""},
    {a: null},
    {b: 1},
    {a: [1, 2]},
    {a: [2, 3]},
    {a: [[1, 2]]},
]));

assertArrayEq({
    actual: c.find({a: {$ne: 1}}, {_id: 0}).toArray(),
    expected: [
        {a: 2},
        {a: ""},
        {a: null},
        {b: 1},
        {a: [2, 3]},
        {a: [[1, 2]]},
    ],
});

const res = c.explain("executionStats").aggregate({$match: {a: {$ne: 1}}});
assertValueOnPlanPath("Filter", res, "child.nodeType");
assertValueOnPlanPath("UnaryOp", res, "child.filter.nodeType");
assertValueOnPlanPath("Not", res, "child.filter.op");

assertArrayEq({
    actual: c.find({a: {$not: {$gt: 1}}}, {_id: 0}).toArray(),
    expected: [
        {a: 1},
        {a: ""},
        {a: null},
        {b: 1},
        {a: [[1, 2]]},
    ],
});

// Show that {$not: {$gt: ...} is different from {$lte: ...}
assertArrayEq({
    actual: c.find({a: {$lte: 1}}, {_id: 0}).toArray(),
    expected: [
        {a: 1},
        {a: [1, 2]},
    ],
});

assertArrayEq({
    actual: c.find({a: {$ne: null}}, {_id: 0}).toArray(),
    expected: [
        {a: 1},
        {a: 2},
        {a: ""},
        {a: [1, 2]},
        {a: [2, 3]},
        {a: [[1, 2]]},
    ],
});

c.drop();

assert.commandWorked(c.insertMany([
    {a: {b: 1}},
    {a: {b: 2}},
    {a: 1},
    {b: 1},
    {a: [{b: 1}, {b: 1}]},
    {a: [{b: 1}, {b: 2}]},
    {a: [{b: 2}, {b: 3}]},
    {a: [{b: 2}, {b: null}]},
    {a: [{b: 2}, {c: "str"}]},
]));

assertArrayEq({
    actual: c.find({"a.b": {$ne: 1}}, {_id: 0}).toArray(),
    expected: [
        {a: {b: 2}},
        {a: 1},
        {b: 1},
        {a: [{b: 2}, {b: 3}]},
        {a: [{b: 2}, {b: null}]},
        {a: [{b: 2}, {c: "str"}]},
    ],
});

assertArrayEq({
    actual: c.find({"a": {$elemMatch: {b: {$ne: 1}}}}, {_id: 0}).toArray(),
    expected: [
        {a: [{b: 1}, {b: 2}]},
        {a: [{b: 2}, {b: 3}]},
        {a: [{b: 2}, {b: null}]},
        {a: [{b: 2}, {c: "str"}]},
    ],
});

assertArrayEq({
    actual: c.find({"a": {$not: {$elemMatch: {b: 1}}}}, {_id: 0}).toArray(),
    expected: [
        {a: {b: 1}},
        {a: {b: 2}},
        {a: 1},
        {b: 1},
        {a: [{b: 2}, {b: 3}]},
        {a: [{b: 2}, {b: null}]},
        {a: [{b: 2}, {c: "str"}]},
    ],
});

c.drop();

assert.commandWorked(c.insertMany([
    {a: [1, 5]},
    {a: [3, 7]},
]));

assertArrayEq({
    actual: c.find({a: {$elemMatch: {$not: {$gt: 4}, $gt: 2}}}, {_id: 0}).toArray(),
    expected: [{a: [3, 7]}],
});

assertArrayEq({
    actual: c.find({a: {$elemMatch: {$not: {$gt: 5, $lt: 8}, $gt: 4}}}, {_id: 0}).toArray(),
    expected: [{a: [1, 5]}],
});

c.drop();

assert.commandWorked(c.insertMany([
    {a: 1},
    {a: null},
    {a: 2},
    {a: 3},
    {a: [1, 2]},
    {a: [2, 3]},
    {a: [3, 4]},
    {b: 1},
]));

assertArrayEq({
    actual: c.find({a: {$nin: [1, 2]}}, {_id: 0}).toArray(),
    expected: [
        {a: null},
        {a: 3},
        {a: [3, 4]},
        {b: 1},
    ],
});

assertArrayEq({
    actual: c.find({a: {$nin: [1, null]}}, {_id: 0}).toArray(),
    expected: [
        {a: 2},
        {a: 3},
        {a: [2, 3]},
        {a: [3, 4]},
    ],
});

assertArrayEq({
    actual: c.find({a: {$not: {$elemMatch: {$gte: 1, $lte: 2}}}}, {_id: 0}).toArray(),
    expected: [
        {a: 1},
        {a: null},
        {a: 2},
        {a: 3},
        {a: [3, 4]},
        {b: 1},
    ],
});

c.drop();

assert.commandWorked(c.insertMany([
    {a: 1},
    {a: null},
    {a: [1, 2]},
    {b: 1},
    {b: null},
]));

assertArrayEq({
    actual: c.find({a: {$not: {$exists: true}}}, {_id: 0}).toArray(),
    expected: [
        {b: 1},
        {b: null},
    ],
});

assertArrayEq({
    actual: c.find({a: {$exists: false}}, {_id: 0}).toArray(),
    expected: [
        {b: 1},
        {b: null},
    ],
});

assertArrayEq({
    actual: c.find({a: {$not: {$exists: false}}}, {_id: 0}).toArray(),
    expected: [
        {a: 1},
        {a: null},
        {a: [1, 2]},
    ],
});

c.drop();

assert.commandWorked(c.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 2},
    {a: [2, 3], b: 2},
]));

assertArrayEq({
    actual: c.find({$and: [{a: {$ne: 1}}, {b: 2}]}, {_id: 0}).toArray(),
    expected: [
        {a: 2, b: 2},
        {a: [2, 3], b: 2},
    ],
});

// Test that position of $not under $and doesn't affect results.
assertArrayEq({
    actual: c.find({$and: [{b: 2}, {a: {$ne: 1}}]}, {_id: 0}).toArray(),
    expected: [
        {a: 2, b: 2},
        {a: [2, 3], b: 2},
    ],
});

assertArrayEq({
    actual: c.find({$or: [{a: {$ne: 1}}, {b: 1}]}, {_id: 0}).toArray(),
    expected: [
        {a: 1, b: 1},
        {a: 2, b: 2},
        {a: [2, 3], b: 2},
    ],
});

c.drop();

assert.commandWorked(c.insertMany([
    {a: [[1, 2], [3, 4]]},
    {a: [[3, 3], [3, 3]]},
    {a: [[1, 2], [4, 4]]},
]));

assertArrayEq({
    actual: c.find({a: {$elemMatch: {$elemMatch: {$ne: 3}}}}, {_id: 0}).toArray(),
    expected: [
        {a: [[1, 2], [3, 4]]},
        {a: [[1, 2], [4, 4]]},
    ],
});

assertArrayEq({
    actual: c.find({a: {$elemMatch: {$not: {$elemMatch: {$ne: 3}}}}}, {_id: 0}).toArray(),
    expected: [
        {a: [[3, 3], [3, 3]]},
    ],
});
}());
