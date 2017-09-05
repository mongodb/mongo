// Tests that the $changeStream returns an error when run against a sharded collection.
// TODO SERVER-29141 Add support for sending a $changeStream to a sharded collection.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode().

    const shardingTest = new ShardingTest({nodes: 1, rs: {nodes: 1}, config: 1});

    const testDB = shardingTest.getDB("test");

    // Test that $changeStream is disallowed on a sharded collection.
    assert(shardingTest.adminCommand({enableSharding: "test"}));
    assert(shardingTest.adminCommand({shardCollection: "test.sharded", key: {_id: 1}}));
    const shardedCollection = testDB.getCollection("sharded");

    assertErrorCode(shardedCollection, [{$changeStream: {}}], 40622);

    // Test that it is still disallowed even if it's not specified as the first stage.
    assertErrorCode(shardedCollection, [{$project: {_id: 0}}, {$changeStream: {}}], 40622);

    shardingTest.stop();
}());
