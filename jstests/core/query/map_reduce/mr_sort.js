// Cannot implicitly shard accessed collections because the "limit" option to the "mapReduce"
// command cannot be used on a sharded collection.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
//   requires_scripting,
// ]

let t = db.mr_sort;
t.drop();

t.createIndex({x: 1});

t.insert({x: 1});
t.insert({x: 10});
t.insert({x: 2});
t.insert({x: 9});
t.insert({x: 3});
t.insert({x: 8});
t.insert({x: 4});
t.insert({x: 7});
t.insert({x: 5});
t.insert({x: 6});

let m = function () {
    emit("a", this.x);
};

let r = function (k, v) {
    return Array.sum(v);
};

let out = db.mr_sort_out;
assert.commandWorked(t.mapReduce(m, r, out.getName()));
assert.eq([{_id: "a", value: 55}], out.find().toArray(), "A1");
out.drop();

assert.commandWorked(t.mapReduce(m, r, {out: "mr_sort_out", query: {x: {$lt: 3}}}));
assert.eq([{_id: "a", value: 3}], out.find().toArray(), "A2");
out.drop();

assert.commandWorked(t.mapReduce(m, r, {out: "mr_sort_out", sort: {x: 1}, limit: 2}));
assert.eq([{_id: "a", value: 3}], out.find().toArray(), "A3");
out.drop();

// Verify that specifying a sort with no limit succeeds.
assert.commandWorked(t.mapReduce(m, r, {out: "mr_sort_out", sort: {x: 1}}));
assert.eq([{_id: "a", value: 55}], out.find().toArray());
assert(out.drop());
