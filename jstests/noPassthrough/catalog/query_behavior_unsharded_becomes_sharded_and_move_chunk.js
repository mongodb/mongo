/**
 * Tests the behavior of a query that started targeting an unsharded collection and that collection
 * becomes sharded, a chunk is moved and, consequently, a range deletion is executed.
 *   - If the query runs against a primary node, the query should hold the range deletion task.
 *   - Contrarily, on a secondary node, the query should get killed when the range deletion
 *     is processed.
 *
 * @tags: [
 *   requires_fcv_82
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 2},
    other: {rsOptions: {setParameter: {orphanCleanupDelaySecs: 0}}}
});

const db = st.s.getDB("query_gets_killed_test");
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

// Set shard0 as the primary shard for `db`
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: shard0}));

let collId = 0;
function getNewColl(db) {
    return db.getCollection("coll_" + collId++);
}

{
    jsTestLog("Running a query on the primary node must hold the range deletion");

    const coll = getNewColl(db);

    // Create an unsharded collection
    assert.commandWorked(db.createCollection(coll.getName()));

    // Create an index to be able to shard the collection
    assert.commandWorked(coll.createIndex({x: 1}));

    // Insert a lot of documents
    let insertions = [];
    for (let i = 0; i < 100; ++i) {
        insertions.push({x: i});
    }
    coll.insertMany(insertions);

    // Start a query
    const cursor = new DBCommandCursor(
        db, assert.commandWorked(db.runCommand({find: coll.getName(), filter: {}, batchSize: 5})));

    // Shard the collection and move a chunk out of the primary shard to force a
    // range deletion.
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand(
        {moveRange: coll.getFullName(), min: {x: MinKey()}, max: {x: MaxKey()}, toShard: shard1}));

    // The ongoing query should hold the range deletion task.
    sleep(500);
    assert.eq(1, st.shard0.getDB("config").getCollection("rangeDeletions").find().toArray().length);

    // Exhaust the cursor.
    cursor.toArray();

    // The range deletion should get executed eventually once the query gets complete.
    assert.soon(() => {
        return st.shard0.getDB("config").getCollection("rangeDeletions").find().itcount() == 0;
    });
}

{
    jsTestLog(
        "Running a query on a secondary node. The query must get killed the query on a range deletion");

    const coll = getNewColl(db);

    // Create an unsharded collection
    assert.commandWorked(db.createCollection(coll.getName()));

    // Create an index to be able to shard the collection
    assert.commandWorked(coll.createIndex({x: 1}));

    // Insert a lot of documents
    let insertions = [];
    for (let i = 0; i < 100; ++i) {
        insertions.push({x: i});
    }
    coll.insertMany(insertions);

    // Start a query
    const cursor = assert
                       .commandWorked(db.runCommand({
                           find: coll.getName(),
                           $readPreference: {mode: "secondary"},
                           readConcern: {level: 'local'},
                           filter: {},
                           batchSize: 5
                       }))
                       .cursor;

    // Shard the collection and move a chunk out of the primary shard to force a
    // range deletion.
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand(
        {moveRange: coll.getFullName(), min: {x: MinKey()}, max: {x: MaxKey()}, toShard: shard1}));

    // Wait for the range deletion to be executed.
    assert.soon(() => {
        return st.shard0.getDB("config").getCollection("rangeDeletions").find().itcount() == 0;
    });

    // The query should get killed due to the range deletion on shard0.
    assert.commandFailedWithCode(
        db.runCommand({getMore: cursor.id, collection: coll.getName(), batchSize: 10}),
        ErrorCodes.QueryPlanKilled,
    );
}

st.stop();
