/**
 * Tests change streams in a cluster running an older version of mongos, and assures that no
 * 'migrateChunkToNewShard' events are ever returned to a change stream consumer.
 * Tests opening a change stream on the database through mongos, and also opening a change stream on
 * each shard using the '$_passthroughToShard' option.
 * @tags: [requires_change_streams, requires_sharding]
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {assertNoChanges, assertChangeStreamEventEq} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    config: 1,
    other: {
        // Use an old version of mongos.
        mongosOptions: {binVersion: "last-lts"},
        configOptions: {
            binVersion: "latest",
        },
        // Use the newest version of mongod for the data shards.
        rsOptions: {
            binVersion: "latest",
        },
        rs: {
            nodes: 1,
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
        },
    },
});

const assertNextEvent = (cursor, expectedEvent) => {
    assert.soon(() => cursor.hasNext());
    const changeEvent = cursor.next();
    assertChangeStreamEventEq(changeEvent, expectedEvent);
};

const openCursor = (obj, options = {}) => {
    return obj.watch(
        [],
        Object.assign(
            {
                showExpandedEvents: true,
                batchSize: 0,
            },
            options,
        ),
    );
};

const dbName = jsTestName();
const collName = "coll";

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
const db = st.s.getDB(dbName);

const coll = db[collName];
const ns = {db: dbName, coll: collName};

// Open a cursor on the database, and one cursor on each of the 2 shards using $_passthroughToShard.
let dbCursor = openCursor(db, {});
let dbCursor0 = openCursor(db, {$_passthroughToShard: {shard: st.shard0.shardName}});
let dbCursor1 = openCursor(db, {$_passthroughToShard: {shard: st.shard1.shardName}});

// Open a cursors on the collection on the specific shards using $_passthroughToShard.
let collCursor0 = openCursor(coll, {$_passthroughToShard: {shard: st.shard0.shardName}});
let collCursor1 = openCursor(coll, {$_passthroughToShard: {shard: st.shard1.shardName}});

// Create a collection on only the primary shard and insert 2 documents.
// The events should be visible only on shard0.
assert.commandWorked(coll.insert({_id: 0}));
assert.commandWorked(coll.insert({_id: 1}));

assertNextEvent(dbCursor, {operationType: "create", ns, nsType: "collection"});
assertNextEvent(dbCursor0, {operationType: "create", ns, nsType: "collection"});
assertNextEvent(collCursor0, {operationType: "create", ns, nsType: "collection"});

assertNextEvent(dbCursor, {operationType: "insert", ns, documentKey: {_id: 0}, fullDocument: {_id: 0}});
assertNextEvent(dbCursor0, {operationType: "insert", ns, documentKey: {_id: 0}, fullDocument: {_id: 0}});
assertNextEvent(collCursor0, {operationType: "insert", ns, documentKey: {_id: 0}, fullDocument: {_id: 0}});

assertNextEvent(dbCursor, {operationType: "insert", ns, documentKey: {_id: 1}, fullDocument: {_id: 1}});
assertNextEvent(dbCursor0, {operationType: "insert", ns, documentKey: {_id: 1}, fullDocument: {_id: 1}});
assertNextEvent(collCursor0, {operationType: "insert", ns, documentKey: {_id: 1}, fullDocument: {_id: 1}});

// No changes to shard1 until here.
assertNoChanges(dbCursor1);
assertNoChanges(collCursor1);

// Split the collection into 2 shards.
st.shardColl(collName, {_id: 1}, {_id: 0}, {_id: 0}, dbName);

// The shardCollection command should be visible only on shard0.
assertNextEvent(dbCursor, {operationType: "shardCollection", ns});
assertNextEvent(dbCursor0, {operationType: "shardCollection", ns});
assertNextEvent(collCursor0, {operationType: "shardCollection", ns});

// Insert documents on the 2 shards. These should be visible on the 2 shards.
assert.commandWorked(coll.insert({_id: -1}));
assert.commandWorked(coll.insert({_id: 2}));

assertNextEvent(dbCursor, {operationType: "insert", ns, documentKey: {_id: -1}, fullDocument: {_id: -1}});
assertNextEvent(dbCursor0, {operationType: "insert", ns, documentKey: {_id: -1}, fullDocument: {_id: -1}});
assertNextEvent(collCursor0, {operationType: "insert", ns, documentKey: {_id: -1}, fullDocument: {_id: -1}});
assertNextEvent(dbCursor, {operationType: "insert", ns, documentKey: {_id: 2}, fullDocument: {_id: 2}});
assertNextEvent(dbCursor1, {operationType: "insert", ns, documentKey: {_id: 2}, fullDocument: {_id: 2}});
assertNextEvent(collCursor1, {operationType: "insert", ns, documentKey: {_id: 2}, fullDocument: {_id: 2}});

// Reshard the collection. This should not emit any events.
assert.commandWorked(st.s.adminCommand({reshardCollection: coll.getFullName(), key: {_id: 1}, numInitialChunks: 2}));
assertNoChanges(dbCursor);
assertNoChanges(dbCursor0);
assertNoChanges(dbCursor1);
assertNoChanges(collCursor0);
assertNoChanges(collCursor1);

// Unshard the collection. This should emit an event only on the previous primary shard (shard0).
assert.commandWorked(st.s.adminCommand({unshardCollection: coll.getFullName(), toShard: st.shard1.shardName}));
assertNextEvent(dbCursor, {
    operationType: "reshardCollection",
    ns,
});

assertNextEvent(dbCursor0, {
    operationType: "reshardCollection",
    ns,
});

assertNextEvent(collCursor0, {
    operationType: "reshardCollection",
    ns,
});

// Move the primary from shard1 to shard0. This does not emit any events.
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

assertNoChanges(dbCursor);
assertNoChanges(dbCursor0);
assertNoChanges(dbCursor1);
assertNoChanges(collCursor0);
assertNoChanges(collCursor1);

// Remove the unnecessary shard. This does not emit any events.
assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));

assertNoChanges(dbCursor);
assertNoChanges(dbCursor0);
assertNoChanges(dbCursor1);
assertNoChanges(collCursor0);
assertNoChanges(collCursor1);

dbCursor.close();
dbCursor0.close();
dbCursor1.close();
collCursor0.close();
collCursor1.close();

st.stop();
