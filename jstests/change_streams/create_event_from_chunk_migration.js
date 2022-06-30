/**
 * Test that change streams returns 'create' and 'createIndexes' events from chunk migration when
 * showSystemEvents is set.
 *
 *  @tags: [
 *    requires_fcv_60,
 *    requires_sharding,
 *    uses_change_streams,
 *    change_stream_does_not_expect_txns,
 *    assumes_unsharded_collection,
 *    assumes_read_preference_unchanged,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.
const dbName = jsTestName();
const collName = "test";
const collNS = dbName + "." + collName;
const ns = {
    db: dbName,
    coll: collName
};
const numDocs = 1;

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;
const db = mongosConn.getDB(dbName);
const test = new ChangeStreamTest(db);

function getCollectionUuid(coll) {
    const collInfo = db.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

function assertMigrateEventObserved(cursor, expectedEvent) {
    let events = test.getNextChanges(cursor, 1);
    let event = events[0];
    // Check the presence and the type of 'wallTime' field. We have no way to check the correctness
    // of 'wallTime' value, so we delete it afterwards.
    assert(event.wallTime instanceof Date);
    delete event.wallTime;
    expectedEvent.collectionUUID = getCollectionUuid(collName);
    assertChangeStreamEventEq(event, expectedEvent);
    return event._id;
}

function prepareCollection() {
    assertDropCollection(db, collName);
    assert.commandWorked(db.runCommand({create: collName}));
    assert.commandWorked(
        db.runCommand({createIndexes: collName, indexes: [{key: {x: 1}, name: "idx_x"}]}));

    assert.commandWorked(st.s.adminCommand({shardCollection: collNS, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: collNS, middle: {_id: 0}}));
}

// Test that create and createIndexes events are observable with migration.
function validateCreateEventsFromChunkMigration() {
    prepareCollection();
    let pipeline = [
        {$changeStream: {showExpandedEvents: true, showSystemEvents: true}},
    ];

    let cursor = test.startWatchingChanges({pipeline, collection: collName});

    assert.commandWorked(
        db.adminCommand({moveChunk: collNS, find: {_id: 0}, to: st.shard1.shardName}));

    test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "create",
            ns: ns,
        }
    });

    test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "createIndexes",
            ns: ns,
        }
    });
}

// Test that if showSystemEvents is false, we do not see the create and createIndexes events from
// chunk migration.
function validateShowSystemEventsFalse() {
    prepareCollection();
    let pipeline = [
        {$changeStream: {showExpandedEvents: true, showSystemEvents: false}},
    ];
    let cursor = test.startWatchingChanges({pipeline, collection: collName});

    assert.commandWorked(
        db.adminCommand({moveChunk: collNS, find: {_id: 0}, to: st.shard1.shardName}));

    assert.commandWorked(db[collName].insert({_id: 1, x: 1}));

    // Confirm that we don't observe the create event in the stream, but only see
    // the subsequent insert.
    test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 1, x: 1},
            documentKey: {_id: 1},
        }
    });
}

assert.commandWorked(db.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

validateCreateEventsFromChunkMigration();
validateShowSystemEventsFalse();

st.stop();
}());
