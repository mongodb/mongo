// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]

t = db.mr_index2;
t.drop();

t.save({arr: [1, 2]});

map = function() {
    emit(this._id, 1);
};
reduce = function(k, vals) {
    return Array.sum(vals);
};

res = assert.commandWorked(t.mapReduce(map, reduce, {out: "mr_index2_out", query: {}}));
assert.eq(1, res.counts.input, "A");
db.mr_index2_out.drop();

res =
    assert.commandWorked(t.mapReduce(map, reduce, {out: "mr_index2_out", query: {arr: {$gte: 0}}}));
assert.eq(1, res.counts.input, "B");
db.mr_index2_out.drop();

t.ensureIndex({arr: 1});
res =
    assert.commandWorked(t.mapReduce(map, reduce, {out: "mr_index2_out", query: {arr: {$gte: 0}}}));
assert.eq(1, res.counts.input, "C");
db.mr_index2_out.drop();
