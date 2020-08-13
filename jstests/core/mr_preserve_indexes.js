// Tests that mapReduce preserves the indexes of the output collection, even when replacing it.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   sbe_incompatible,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";
const coll = db.mr_preserve_indexes;
coll.drop();

const outName = "mr_preserve_indexes_out";
const out = db[outName];
out.drop();

assert.commandWorked(coll.insert([
    {tags: [1]},
    {tags: [1, 2]},
    {tags: [1, 2, 3]},
    {tags: [3]},
    {tags: [2, 3]},
    {tags: [2, 3]},
    {tags: [1, 2]}
]));

const mapFn = function() {
    for (let tag of this.tags)
        emit(tag, 1);
};

const reduceFn = function(k, vs) {
    return Array.sum(vs);
};

assert.commandWorked(coll.mapReduce(mapFn, reduceFn, {out: outName}));

assert.eq(1, out.getIndexes().length, () => tojson(out.getIndexes()));
assert.commandWorked(out.createIndex({value: 1}));
assert.eq(2, out.getIndexes().length, () => tojson(out.getIndexes()));

assert.commandWorked(coll.mapReduce(mapFn, reduceFn, {out: outName}));

assert.eq(2, out.getIndexes().length, () => tojson(out.getIndexes()));
}());
