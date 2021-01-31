// Tests the 'reduce' output mode for mapReduce.
// Cannot implicitly shard accessed collections because of following errmsg: Cannot output to a
// non-sharded collection because sharded collection exists already.
// @tags: [
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";
(function() {
const source = db.mr_reduce;
source.drop();

assert.commandWorked(source.insert({_id: 1, a: [1, 2]}));
assert.commandWorked(source.insert({_id: 2, a: [2, 3]}));
assert.commandWorked(source.insert({_id: 3, a: [3, 4]}));

const out = db.mr_reduce_out;
const outName = out.getName();
out.drop();

const map = function() {
    for (let i = 0; i < this.a.length; i++)
        emit(this.a[i], 1);
};
const reduce = function(k, vs) {
    return Array.sum(vs);
};

assert.commandWorked(source.mapReduce(map, reduce, {out: outName}));

let expected = [{_id: 1, value: 1}, {_id: 2, value: 2}, {_id: 3, value: 2}, {_id: 4, value: 1}];
assert.docEq(expected, out.find().sort({_id: 1}).toArray());

assert.commandWorked(source.insert({_id: 4, a: [4, 5]}));
// Insert something that should be unaltered by the mapReduce into the output collection.
assert.commandWorked(out.insert({_id: 10, value: 5}));
assert.commandWorked(
    source.mapReduce(map, reduce, {out: {reduce: outName}, query: {_id: {$gt: 3}}}));

expected = [
    {_id: 1, value: 1},
    {_id: 2, value: 2},
    {_id: 3, value: 2},
    {_id: 4, value: 2},
    {_id: 5, value: 1},
    {_id: 10, value: 5}
];
assert.docEq(expected, out.find().sort({_id: 1}).toArray());

assert.commandWorked(source.insert({_id: 5, a: [5, 6]}));
// Insert something that should be unaltered by the mapReduce into the output collection.
assert.commandWorked(out.insert({_id: 20, value: 10}));
assert.commandWorked(
    source.mapReduce(map, reduce, {out: {reduce: outName}, query: {_id: {$gt: 4}}}));

expected = [
    {_id: 1, value: 1},
    {_id: 2, value: 2},
    {_id: 3, value: 2},
    {_id: 4, value: 2},
    {_id: 5, value: 2},
    {_id: 6, value: 1},
    {_id: 10, value: 5},
    {_id: 20, value: 10}
];
assert.docEq(expected, out.find().sort({_id: 1}).toArray());
}());
(function() {
const source = db.mr_reduce;
source.drop();

assert.commandWorked(source.insert({_id: 1, x: 1}));
assert.commandWorked(source.insert({_id: 2, x: 1}));
assert.commandWorked(source.insert({_id: 3, x: 2}));

const out = db.mr_reduce_out;
const outName = out.getName();
out.drop();

const map = function() {
    emit(this.x, 1);
};
const reduce = function(k, v) {
    return Array.sum(v);
};

assert.commandWorked(
    source.mapReduce(map, reduce, {out: {reduce: outName}, query: {_id: {$gt: 0}}}));

assert.eq(2, out.findOne({_id: 1}).value);
assert.eq(1, out.findOne({_id: 2}).value);

assert.commandWorked(source.insert({_id: 4, x: 2}));
assert.commandWorked(
    source.mapReduce(map, reduce, {out: {reduce: outName}, query: {_id: {$gt: 3}}}));

assert.eq(2, out.findOne({_id: 1}).value);
assert.eq(2, out.findOne({_id: 2}).value);
}());
}());
