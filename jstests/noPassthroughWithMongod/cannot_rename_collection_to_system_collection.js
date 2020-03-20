/**
 * Verifies that we disallow renaming collections to system collections and vice-versa.
 */
(function() {
"use strict";

const dbName = "cannot_rename_collection_to_system_collection";

const testDB = db.getSiblingDB(dbName);

assert.commandFailedWithCode(testDB.createCollection("system.a"), ErrorCodes.InvalidNamespace);

testDB.getCollection("a").drop();
assert.commandWorked(testDB.createCollection("a"));

// Ensure we cannot rename "a" to a system collections
assert.commandFailedWithCode(testDB.getCollection("a").renameCollection("system.a"),
                             ErrorCodes.IllegalOperation);

// Ensure we cannot rename system collections to "a".
assert.commandFailedWithCode(
    db.getSiblingDB("admin").getCollection("system.version").renameCollection("a"),
    ErrorCodes.IllegalOperation);
}());
