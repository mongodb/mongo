// Test for SERVER-7017
// We shouldn't rename a collection when one of its indexes will generate a namespace
// that is greater than 120 chars. To do this we create a long index name and try
// and rename the collection to one with a much longer name. We use the test database
// by default and we add this here to ensure we are using it
testDB = db.getSiblingDB("test");
c = "rename2c";
dbc = testDB.getCollection(c);
d = "dest4567890123456789012345678901234567890123456789012345678901234567890";
dbd = testDB.getCollection(d);
dbc.ensureIndex({
    "name": 1,
    "date": 1,
    "time": 1,
    "renameCollection": 1,
    "mongodb": 1,
    "testing": 1,
    "data": 1
});
// Checking for the newly created index and the _id index in original collection
assert.eq(2, dbc.getIndexes().length, "Long Rename Init");
// Should fail to rename collection as the index namespace is too long
assert.commandFailed(dbc.renameCollection(d), "Long Rename Exec");
// Since we failed we should have the 2 indexes unmoved and no indexes under the new collection name
assert.eq(2, dbc.getIndexes().length, "Long Rename Result 1");
assert.eq(0, dbd.getIndexes().length, "Long Rename Result 2");
