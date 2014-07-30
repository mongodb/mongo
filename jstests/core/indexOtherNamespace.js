// SERVER-8814: Test that only the system.indexes namespace can be used to build indexes.

var otherDB = db.getSiblingDB("indexOtherNS");
otherDB.dropDatabase();

otherDB.foo.insert({a:1})
assert.eq(1, otherDB.foo.getIndexes().length);
assert.eq("BasicCursor", otherDB.foo.find({a:1}).explain().cursor);

assert.writeError(otherDB.randomNS.system.indexes.insert({ ns: "indexOtherNS.foo",
                                                           key: { a: 1 }, name: "a_1"}));

// Assert that index didn't actually get built
assert.eq(1, otherDB.foo.getIndexes().length);
assert.eq("BasicCursor", otherDB.foo.find({a:1}).explain().cursor);
otherDB.dropDatabase();
