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

    const merizos = shardingTest.s;
    const merizosDB = merizos.getDB(dbName);
    const merizosColl = merizosDB[collName];

    // Create and shard collection beforehand.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {_id: 1}}));
    assert.commandWorked(merizosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    const session = merizos.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // Start a transaction and verify that it succeeds.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 0}));
    session.commitTransaction();

    assert.eq({_id: 0}, sessionColl.findOne({_id: 0}));

    shardingTest.stop();
})();
