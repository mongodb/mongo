/**
 * Tests some of the find command's semantics with respect to how arrays are handled.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const t = db.jstests_arrayfind10;
t.drop();

function runWithAndWithoutIndex(keyPattern, testFunc) {
    testFunc();
    assert.commandWorked(t.createIndex(keyPattern));
    testFunc();
}

assert.commandWorked(t.insert([{a: "foo"}, {a: ["foo"]}, {a: [["foo"]]}, {a: [[["foo"]]]}]));

runWithAndWithoutIndex({a: 1}, () => {
    assert(arrayEq(t.find({a: "foo"}, {_id: 0}).toArray(), [{a: "foo"}, {a: ["foo"]}]));
});

assert(t.drop());

assert.commandWorked(t.insert([
    {a: [123, "foo"]},
    {a: ["foo", 123]},
    {a: ["bar", "foo"]},
    {a: ["bar", "baz", "foo"]},
    {a: ["bar", "baz", 123]}
]));

runWithAndWithoutIndex({a: 1}, () => {
    assert(arrayEq(
        t.find({a: "foo"}, {_id: 0}).toArray(),
        [{a: [123, "foo"]}, {a: ["foo", 123]}, {a: ["bar", "foo"]}, {a: ["bar", "baz", "foo"]}]));
});

assert(t.drop());

assert.commandWorked(t.insert([
    {a: [{}, {b: "foo"}]},
    {a: [{b: "foo"}, {}]},
    {a: [{b: 123}, {b: "foo"}]},
    {a: [{b: "foo"}, {b: 123}]},
    {a: [{b: "bar"}, {b: "foo"}]},
    {a: [{b: "bar"}, {b: "baz"}, {b: "foo"}]},
    {a: [{b: "bar"}, {b: "baz"}, {b: 123}]}
]));

runWithAndWithoutIndex({"a.b": 1}, () => {
    assert(arrayEq(t.find({"a.b": "foo"}, {_id: 0}).toArray(), [
        {a: [{}, {b: "foo"}]},
        {a: [{b: "foo"}, {}]},
        {a: [{b: 123}, {b: "foo"}]},
        {a: [{b: "foo"}, {b: 123}]},
        {a: [{b: "bar"}, {b: "foo"}]},
        {a: [{b: "bar"}, {b: "baz"}, {b: "foo"}]}
    ]));
});

assert(t.drop());

assert.commandWorked(
    t.insert([{"a": [{"b": [{"c": [5, 7]}]}]}, {"a": [{"b": []}, {"b": [{"c": [5, 7]}]}]}]));

runWithAndWithoutIndex({"a.b": 1}, () => {
    assert(arrayEq(t.find({"a.b.c": 5}, {_id: 0}).toArray(),
                   [{"a": [{"b": [{"c": [5, 7]}]}]}, {"a": [{"b": []}, {"b": [{"c": [5, 7]}]}]}]));
    assert(arrayEq(t.find({"a.b.c": {$eq: 5}}, {_id: 0}).toArray(),
                   [{"a": [{"b": [{"c": [5, 7]}]}]}, {"a": [{"b": []}, {"b": [{"c": [5, 7]}]}]}]));

    assert(arrayEq(t.find({"a.b.c": {$in: [5]}}, {_id: 0}).toArray(),
                   [{"a": [{"b": [{"c": [5, 7]}]}]}, {"a": [{"b": []}, {"b": [{"c": [5, 7]}]}]}]));

    assert(arrayEq(t.find({"a.b.c": {$size: 2}}, {_id: 0}).toArray(),
                   [{"a": [{"b": [{"c": [5, 7]}]}]}, {"a": [{"b": []}, {"b": [{"c": [5, 7]}]}]}]));

    assert(arrayEq(t.find({"a.b.c": {$elemMatch: {$eq: 5}}}, {_id: 0}).toArray(),
                   [{"a": [{"b": [{"c": [5, 7]}]}]}, {"a": [{"b": []}, {"b": [{"c": [5, 7]}]}]}]));
});

assert(t.drop());

assert.commandWorked(t.insert([
    {a: 1},
    {a: 2},
    {a: 3},
    {a: [1]},
    {a: [1, 2]},
    {a: [2]},
    {a: [3]},
    {a: [3, 1]},
    {a: [3, 2]},
    {a: [3, 2, 1]},
    {a: [[1]]},
    {a: [[2]]},
    {a: [[3]]},
    {a: [[[1]]]},
    {a: [[[2]]]},
    {a: [[[3]]]},
    {a: [1, [[2]]]},
    {a: [3, [1]]},
    {a: [3, [[2]]]},
    {a: [3, [[2]], 1]},
    {a: [[1], 2]},
    {a: [[1], [2]]},
    {a: [[3], 1]},
    {a: [[3], 2]},
    {a: [[3], 2, 1]},
    {a: [[3], [1]]},
    {a: [[3], [[2]]]},
    {a: [[3], [[2]], 1]}
]));

runWithAndWithoutIndex({a: 1}, () => {
    assert(arrayEq(t.find({a: {$eq: [2]}}, {_id: 0}).toArray(),
                   [{a: [2]}, {a: [[2]]}, {a: [[1], [2]]}]));

    assert(arrayEq(t.find({a: {$lt: [2]}}, {_id: 0}).toArray(), [
        {a: [1]},
        {a: [1, 2]},
        {a: [[1]]},
        {a: [1, [[2]]]},
        {a: [3, [1]]},
        {a: [[1], 2]},
        {a: [[1], [2]]},
        {a: [[3], [1]]}
    ]));

    assert(arrayEq(t.find({a: {$gt: [2]}}, {_id: 0}).toArray(), [
        {a: [3]},          {a: [3, 1]},         {a: [3, 2]},      {a: [3, 2, 1]},
        {a: [[1]]},        {a: [[2]]},          {a: [[3]]},       {a: [[[1]]]},
        {a: [[[2]]]},      {a: [[[3]]]},        {a: [1, [[2]]]},  {a: [3, [1]]},
        {a: [3, [[2]]]},   {a: [3, [[2]], 1]},  {a: [[1], 2]},    {a: [[1], [2]]},
        {a: [[3], 1]},     {a: [[3], 2]},       {a: [[3], 2, 1]}, {a: [[3], [1]]},
        {a: [[3], [[2]]]}, {a: [[3], [[2]], 1]}
    ]));

    assert(arrayEq(t.find({a: {$eq: [1, 2]}}, {_id: 0}).toArray(), [{a: [1, 2]}]));

    assert(arrayEq(
        t.find({a: {$lt: [1, 2]}}, {_id: 0}).toArray(),
        [{a: [1]}, {a: [[1]]}, {a: [3, [1]]}, {a: [[1], 2]}, {a: [[1], [2]]}, {a: [[3], [1]]}]));

    assert(arrayEq(t.find({a: {$gt: [1, 2]}}, {_id: 0}).toArray(), [
        {a: [2]},        {a: [3]},          {a: [3, 1]},         {a: [3, 2]},
        {a: [3, 2, 1]},  {a: [[1]]},        {a: [[2]]},          {a: [[3]]},
        {a: [[[1]]]},    {a: [[[2]]]},      {a: [[[3]]]},        {a: [1, [[2]]]},
        {a: [3, [1]]},   {a: [3, [[2]]]},   {a: [3, [[2]], 1]},  {a: [[1], 2]},
        {a: [[1], [2]]}, {a: [[3], 1]},     {a: [[3], 2]},       {a: [[3], 2, 1]},
        {a: [[3], [1]]}, {a: [[3], [[2]]]}, {a: [[3], [[2]], 1]}
    ]));
});

assert(t.drop());

assert.commandWorked(t.insert([
    {a: [1]},
    {a: [1, 1]},
    {a: [1, 2]},
    {a: [1, 2, 3]},
    {a: [1, 3]},
    {a: [2]},
    {a: [2, 1]},
    {a: [2, 1, 3]},
    {a: [2, 2]},
    {a: [2, 3]},
    {a: [3, 1]},
    {a: [3, 2]},
    {a: [3, 3]},
]));

runWithAndWithoutIndex({a: 1}, () => {
    assert.eq(1, t.find({a: {$eq: [2, 2]}}).itcount());
    assert.eq(8, t.find({a: {$lt: [2, 2]}}).itcount());
    assert.eq(9, t.find({a: {$lte: [2, 2]}}).itcount());
    assert.eq(4, t.find({a: {$gt: [2, 2]}}).itcount());
    assert.eq(5, t.find({a: {$gte: [2, 2]}}).itcount());

    assert.eq(1, t.find({a: {$eq: [1]}}).itcount());
    assert.eq(0, t.find({a: {$lt: [1]}}).itcount());
    assert.eq(1, t.find({a: {$lte: [1]}}).itcount());
    assert.eq(12, t.find({a: {$gt: [1]}}).itcount());
    assert.eq(13, t.find({a: {$gte: [1]}}).itcount());

    assert.eq(0, t.find({a: {$eq: [3]}}).itcount());
    assert.eq(10, t.find({a: {$lt: [3]}}).itcount());
    assert.eq(10, t.find({a: {$lte: [3]}}).itcount());
    assert.eq(3, t.find({a: {$gt: [3]}}).itcount());
    assert.eq(3, t.find({a: {$gte: [3]}}).itcount());

    assert.eq(1, t.find({a: {$eq: [1, 2, 3]}}).itcount());
    assert.eq(3, t.find({a: {$lt: [1, 2, 3]}}).itcount());
    assert.eq(4, t.find({a: {$lte: [1, 2, 3]}}).itcount());
    assert.eq(9, t.find({a: {$gt: [1, 2, 3]}}).itcount());
    assert.eq(10, t.find({a: {$gte: [1, 2, 3]}}).itcount());
});
})();
