// Tests that the $changeNotification returns an error when run against a mongos.
// TODO SERVER-29141 Add support for sending a $changeNotification to a mongos.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode().

    const shardingTest = new ShardingTest({nodes: 1, rs: {nodes: 1}, config: 1});

    const testDB = shardingTest.getDB("test");

    // Test that $changeNotification is disallowed on an unsharded collection.
    const unshardedCollection = testDB.getCollection("unsharded");

    // Insert a document to ensure the database exists on mongos.
    assert.writeOK(unshardedCollection.insert({}));

    assertErrorCode(unshardedCollection, [{$changeNotification: {}}], 40567);

    // Test that it is still disallowed even if it's not specified as the first stage.
    assertErrorCode(unshardedCollection, [{$project: {_id: 0}}, {$changeNotification: {}}], 40567);

    // Test that $changeNotification is disallowed on a sharded collection.

    assert(shardingTest.adminCommand({enableSharding: "test"}));
    assert(shardingTest.adminCommand({shardCollection: "test.sharded", key: {_id: 1}}));
    const shardedCollection = testDB.getCollection("sharded");

    assertErrorCode(shardedCollection, [{$changeNotification: {}}], 40567);

    // Test that it is still disallowed even if it's not specified as the first stage.
    assertErrorCode(shardedCollection, [{$project: {_id: 0}}, {$changeNotification: {}}], 40567);
}());
