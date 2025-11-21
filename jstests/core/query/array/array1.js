// @tags: [
//   # Time series collections do not support indexing array values in measurement fields.
//   exclude_from_timeseries_crud_passthrough,
// ]

const coll = db.array1;
coll.drop();

const x = {
    a: [1, 2],
};

assert.commandWorked(coll.insert({a: [[1, 2]]}));
assert.eq(1, coll.find(x).count());

assert.commandWorked(coll.insert(x));
assert.eq(2, coll.find(x).count());

assert.commandWorked(coll.createIndex({a: 1}));
assert.eq(2, coll.find(x).count());
