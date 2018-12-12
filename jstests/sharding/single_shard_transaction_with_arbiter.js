/**
 * Tests that single shard transactions succeed against replica sets that contain arbiters.
 *
 * @tags: [uses_transactions, requires_find_command]
 */

(function() {
    "use strict";

    const name = "single_shard_transaction_with_arbiter";
    const dbName = "test";
    const collName = name;

    const shardingTest = new ShardingTest({
        shards: 1,
        rs: {
            nodes: [
                {/* primary */},
                {/* secondary */ rsConfig: {priority: 0}},
                {/* arbiter */ rsConfig: {arbiterOnly: true}}
            ]
        }
    });

    const mongos = shardingTest.s;
    const mongosDB = mongos.getDB(dbName);
    const mongosColl = mongosDB[collName];

    // Create and shard collection beforehand.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));
    assert.commandWorked(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    const session = mongos.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // Start a transaction and verify that it succeeds.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 0}));
    session.commitTransaction();

    assert.eq({_id: 0}, sessionColl.findOne({_id: 0}));

    shardingTest.stop();
})();
