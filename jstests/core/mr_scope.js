// Tests the scope argument to the mapReduce command.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";
const coll = db.mr_scope;
coll.drop();
const outputColl = db.mr_scope_out;
outputColl.drop();

assert.commandWorked(coll.insert([
    {x: 1, tags: ["a", "b"]},
    {x: 2, tags: ["b", "c"]},
    {x: 3, tags: ["c", "a"]},
    {x: 4, tags: ["b", "c"]}
]));

const mapFn = function() {
    this.tags.forEach(tag => {
        emit(tag, {count: xx.val});
    });
};

const reduceFn = function(key, values) {
    let total = 0;
    for (let value of values) {
        total += value.count;
    }
    return {count: total};
};

assert.commandWorked(
    coll.mapReduce(mapFn, reduceFn, {out: {merge: outputColl.getName()}, scope: {xx: {val: 1}}}));

assert.eq(3, outputColl.find().itcount());
assert.eq(1, outputColl.count({_id: "a", "value.count": 2}));
assert.eq(1, outputColl.count({_id: "b", "value.count": 3}));
assert.eq(1, outputColl.count({_id: "c", "value.count": 3}));

outputColl.drop();
assert.commandWorked(
    coll.mapReduce(mapFn, reduceFn, {scope: {xx: {val: 2}}, out: {merge: outputColl.getName()}}));

assert.eq(3, outputColl.find().itcount());
assert.eq(1, outputColl.count({_id: "a", "value.count": 4}));
assert.eq(1, outputColl.count({_id: "b", "value.count": 6}));
assert.eq(1, outputColl.count({_id: "c", "value.count": 6}));

outputColl.drop();
}());
