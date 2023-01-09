// SERVER-5826 ensure you can't build an index with a non-existent plugin
t = db.bad_index_plugin;

assert.commandWorked(t.createIndex({good: 1}));
assert.eq(t.getIndexes().length, 2);  // good + _id

var err = t.createIndex({bad: 'bad'});
assert.commandFailed(err);
assert(err.code >= 0);

assert.eq(t.getIndexes().length, 2);  // good + _id (no bad)
