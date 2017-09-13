// test listIndexes compatability command and system.indexes

t = db.list_indexes2;
t.drop();

t.insert({x: 1});

assert.eq(t._getIndexesSystemIndexes(), t._getIndexesCommand());

t.ensureIndex({x: 1});

assert.eq(t._getIndexesSystemIndexes(), t._getIndexesCommand());

assert.eq(t.getIndexes(), t._getIndexesCommand());
