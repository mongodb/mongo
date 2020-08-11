// Test that the server cleans up correctly when creating a collection fails.
//
// @tags: [requires_capped]
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For 'isMongos()'.

var dbTest = db.getSiblingDB("DB_create_collection_fail_cleanup");
dbTest.dropDatabase();

let collectionNames = dbTest.getCollectionNames();
assert.eq(collectionNames.length, 0, collectionNames);

// This create collection call should fail. It would leave the database in created state though.
assert.commandFailed(dbTest.createCollection("broken", {capped: true, size: -1}));

collectionNames = dbTest.getCollectionNames();
collectionNames.forEach(function(collName) {
    assert.neq(collName, "broken", collectionNames);
});

// Cause a failed collection creation due to an invalid collation. Verify that the failed collection
// does not appear in top output, whereas a successfully created collection does appear. This
// test cannot run against a mongos, since mongos does not support the 'top' command.
if (!FixtureHelpers.isMongos(dbTest)) {
    assert.commandFailed(
        dbTest.createCollection("invalid_collation_collection", {collation: {locale: "invalid"}}));
    assert.commandWorked(
        dbTest.createCollection("legal_collation_collection", {collation: {locale: "en_US"}}));
    const topOutput = dbTest.adminCommand("top");
    printjson(topOutput);
    assert(topOutput.hasOwnProperty("totals"), topOutput);
    assert(!topOutput.totals.hasOwnProperty(dbTest.invalid_collation_collection.getFullName()),
           topOutput);
    assert(topOutput.totals.hasOwnProperty(dbTest.legal_collation_collection.getFullName()),
           topOutput);
}
}());
