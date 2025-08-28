// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop, requires_non_retryable_commands]

// This is a regression test for SERVER-21545.
//
// Tests that a collMod with a bad specification does not cause any changes, and does not crash the
// server.
let collName = "collModBadSpec";
let coll = db.getCollection(collName);

coll.drop();
assert.commandWorked(db.createCollection(collName));

// Get the original collection options for the collection.
let originalResult = db.getCollectionInfos({name: collName});

// Issue an invalid command.
assert.commandFailed(coll.runCommand("collMod", {validationLevel: "off", unknownField: "x"}));

// Make sure the options are unchanged.
let newResult = db.getCollectionInfos({name: collName});
assert.eq(originalResult, newResult);
