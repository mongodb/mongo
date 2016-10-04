// Test the listIndexes command on non-existent collection.
(function() {
    var dbTest = db.getSiblingDB("list_indexes_non_existent_db");
    assert.commandWorked(dbTest.dropDatabase());

    var coll;

    // Non-existent database
    coll = dbTest.getCollection("list_indexes_non_existent_db");
    assert.commandFailed(coll.runCommand("listIndexes"));

    // Creates the actual database that did not exist till now
    coll.insert({});

    // Non-existent collection
    coll = dbTest.getCollection("list_indexes_non_existent_collection");
    assert.commandFailed(coll.runCommand("listIndexes"));
}());
