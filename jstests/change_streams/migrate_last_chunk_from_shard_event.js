/**
 * Test that change streams returns migrateLastChunkFromShard events.
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
    assert.commandWorked(db[collName].insert({_id: 1}));
    assert.commandWorked(st.s.adminCommand({shardCollection: collNS, key: {_id: 1}}));
}

// Test that if showSystemEvents is false, we do not see the migrateLastChunkFromShard event.
function validateShowSystemEventsFalse() {
    prepareCollection();
    let pipeline = [
        {$changeStream: {showExpandedEvents: true, showSystemEvents: false}},
        {$match: {operationType: {$nin: ["create", "createIndexes"]}}}
    ];
    let cursor = test.startWatchingChanges(
        {pipeline, collection: collName, aggregateOptions: {cursor: {batchSize: 0}}});

    // Migrate a chunk, then insert a new document.
    assert.commandWorked(
        db.adminCommand({moveChunk: collNS, find: {_id: 0}, to: st.shard1.shardName}));
    assert.commandWorked(db[collName].insert({_id: numDocs + 1}));

    // Confirm that we don't observe the migrateLastChunkFromShard event in the stream, but only see
    // the subsequent insert.
    test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: numDocs + 1},
            documentKey: {_id: numDocs + 1},
        }
    });
}

// Test that if showSystemEvents is true, we see the migrateLastChunkFromShard event and can resume
// after it.
function validateExpectedEventAndConfirmResumability(collParam, expectedOutput) {
    prepareCollection();

    let pipeline = [
        {$changeStream: {showExpandedEvents: true, showSystemEvents: true}},
        {$match: {operationType: {$nin: ["create", "createIndexes"]}}}
    ];
    let cursor = test.startWatchingChanges(
        {pipeline: pipeline, collection: collParam, aggregateOptions: {cursor: {batchSize: 0}}});

    // Migrate a chunk from one shard to another.
    assert.commandWorked(
        db.adminCommand({moveChunk: collNS, find: {_id: 0}, to: st.shard1.shardName}));

    // Confirm that we observe the migrateLastChunkFromShard event, and obtain its resume token.
    const migrateResumeToken = assertMigrateEventObserved(cursor, expectedOutput);

    // Insert a document before starting the next change stream so that we can validate the
    // resuming behavior.
    assert.commandWorked(db[collName].insert({_id: numDocs + 1}));

    // Resume after the migrate event and confirm we see the subsequent insert.
    pipeline = [{
        $changeStream:
            {showExpandedEvents: true, showSystemEvents: true, resumeAfter: migrateResumeToken}
    }];
    cursor = test.startWatchingChanges({pipeline: pipeline, collection: collParam});

    test.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: numDocs + 1},
            documentKey: {_id: numDocs + 1},
        }
    });
}

assert.commandWorked(db.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

// Test the behaviour of migrateLastChunkFromShard for a single-collection stream
validateExpectedEventAndConfirmResumability(collName, {
    operationType: "migrateLastChunkFromShard",
    ns: ns,
    operationDescription: {
        "shardId": st.shard0.shardName,
    }
});

// Test the behaviour of migrateLastChunkFromShard for a whole-DB stream.
validateExpectedEventAndConfirmResumability(1, {
    operationType: "migrateLastChunkFromShard",
    ns: ns,
    operationDescription: {
        "shardId": st.shard0.shardName,
    }
});

// Test the behaviour of migrateLastChunkFromShard when showSystemEvents is false.
validateShowSystemEventsFalse();

st.stop();
}());
