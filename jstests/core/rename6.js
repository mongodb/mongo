// Test for SERVER-7017
// We are allowed to rename a collection when one of its indexes will generate a namespace
// that is greater than 120 chars. To do this we create a long index name and try
// and rename the collection to one with a much longer name. We use the test database
// by default and we add this here to ensure we are using it
// @tags: [requires_non_retryable_commands, assumes_unsharded_collection]

(function() {
'use strict';

const testDB = db.getSiblingDB("test");
const c = "rename2c";
const dbc = testDB.getCollection(c);
const d = "dest4567890123456789012345678901234567890123456789012345678901234567890";
const dbd = testDB.getCollection(d);

dbc.drop();
dbd.drop();

dbc.ensureIndex({
    "name": 1,
    "date": 1,
    "time": 1,
    "renameCollection": 1,
    "mongodb": 1,
    "testing": 1,
    "data": 1
});

// Checking for the newly created index and the _id index in original collection.
assert.eq(2, dbc.getIndexes().length, "Long Rename Init");
// Should succeed in renaming collection as the long index namespace is acceptable.
assert.commandWorked(dbc.renameCollection(d), "Long Rename Exec");
// Since we succeeded we should have the 2 indexes moved and no indexes under the old collection
// name.
assert.eq(0, dbc.getIndexes().length, "Long Rename Result 1");
assert.eq(2, dbd.getIndexes().length, "Long Rename Result 2");
})();
