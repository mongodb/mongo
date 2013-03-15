// SERVER-5826 ensure you can't build an index with a non-existent plugin
t = db.bad_index_plugin;

assert.eq(t.ensureIndex({good: 1}), undefined);
assert.eq(t.getIndexes().length, 2); // good + _id

err = t.ensureIndex({bad: 'bad'});
assert.neq(err, undefined);
assert.eq(err.code, 16734);

assert.eq(t.getIndexes().length, 2); // good + _id (no bad)
