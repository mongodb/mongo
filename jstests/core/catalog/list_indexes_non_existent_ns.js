/**
 * Tests list indexes on non existent DB.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_on_get_collection,
 * ]
 */
// Test the listIndexes command on non-existent collection.
let dbTest = db.getSiblingDB("list_indexes_non_existent_db");
assert.commandWorked(dbTest.dropDatabase());

let coll;

// Non-existent database
coll = dbTest.getCollection("list_indexes_non_existent_db");
assert.commandFailed(coll.runCommand("listIndexes"));

// Creates the actual database that did not exist till now
coll.insert({});

// Non-existent collection
coll = dbTest.getCollection("list_indexes_non_existent_collection");
assert.commandFailed(coll.runCommand("listIndexes"));
