// Test the listIndexes command on non-existent collection.

var db = db.getSiblingDB("list_indexes_non_existent_db");
assert.commandWorked(db.dropDatabase());

var coll;

// Non-existent database
coll = db.getCollection("list_indexes_non_existent_db");
assert.commandFailed(coll.runCommand("listIndexes"));

// Creates the db
coll.insert({});

// Non-existent collection
coll = db.getCollection("list_indexes_non_existent_collection");
assert.commandFailed(coll.runCommand("listIndexes"));
