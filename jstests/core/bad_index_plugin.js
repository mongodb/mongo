// SERVER-5826 ensure you can't build an index with a non-existent plugin
t = db.bad_index_plugin;

assert.commandWorked(t.ensureIndex({good: 1}));
assert.eq(t.getIndexes().length, 2);  // good + _id

var err = t.ensureIndex({bad: 'bad'});
assert.commandFailed(err);
assert(err.code >= 0);

assert.eq(t.getIndexes().length, 2);  // good + _id (no bad)
