// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
//   requires_fastcount,
//   requires_getmore,
// ]

// SERVER-8514: Test the count command returns an error to the user when given an invalid query
// predicate, even when the collection doesn't exist.

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

var t = db.count11;

t.drop();

// On a sharded cluster if the database doesn't exist, count will return an empty result instead
// of a error.
if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
    // Create database
    assert.commandWorked(db.adminCommand({'enableSharding': db.getName()}));
}

var validQuery = {a: 1};
var invalidQuery = {a: {$invalid: 1}};

// Query non-existing collection with empty query.
assert.eq(0, t.find().count());
assert.eq(0, t.find().itcount());

// Query non-existing collection.
// Returns 0 on valid syntax query.
// Fails on invalid syntax query.
assert.eq(0, t.find(validQuery).count());
assert.throws(function() {
    t.find(invalidQuery).count();
});

// Query existing collection.
// Returns 0 on valid syntax query.
// Fails on invalid syntax query.
assert.commandWorked(db.createCollection(t.getName()));
assert.eq(0, t.find(validQuery).count());
assert.throws(function() {
    t.find(invalidQuery).count();
});
