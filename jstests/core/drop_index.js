
t = db.dropIndex;
t.drop();

t.insert({_id: 1, a: 2, b: 3});
assert.eq(1, t.getIndexes().length, "A1");

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
assert.eq(3, t.getIndexes().length, "A2");

x = db._dbCommand({dropIndexes: t.getName(), index: t._genIndexName({a: 1})});
assert.eq(2, t.getIndexes().length, "B1 " + tojson(x));

x = db._dbCommand({dropIndexes: t.getName(), index: {b: 1}});
assert.eq(1, t.getIndexes().length, "B2");

// ensure you can recreate indexes, even if you don't use dropIndex method
t.ensureIndex({a: 1});
assert.eq(2, t.getIndexes().length);
