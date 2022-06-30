/**
 * Test that change streams returns refineCollectionShardKey events.
 *
 *  @tags: [
 *    requires_fcv_61,
 *    requires_sharding,
 *    uses_change_streams,
 *    change_stream_does_not_expect_txns,
 *    assumes_unsharded_collection,
 *    assumes_read_preference_unchanged,
 *    featureFlagChangeStreamsFurtherEnrichedEvents
 * ]
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.

var st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongos = st.s0;
const primaryShard = st.shard0.shardName;
const kDbName = jsTestName();
const kCollName = 'coll';
const kNsName = kDbName + '.' + kCollName;
const numDocs = 1;

const db = mongos.getDB(kDbName);
const test = new ChangeStreamTest(db);

function getCollectionUuid(coll) {
    const collInfo = db.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

const ns = {
    db: kDbName,
    coll: kCollName
};

function prepareCollection() {
    assertDropCollection(db, kCollName);
    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
    assert.commandWorked(mongos.getCollection(kNsName).createIndex({_id: 1, akey: 1}));
}

function assertExpectedEventObserved(cursor, expectedEvent) {
    let events = test.getNextChanges(cursor, 1);
    let event = events[0];
    // Check the presence and the type of 'wallTime' field. We have no way to check the correctness
    // of 'wallTime' value, so we delete it afterwards.
    assert(event.wallTime instanceof Date);
    delete event.wallTime;
    expectedEvent.collectionUUID = getCollectionUuid(kCollName);
    assertChangeStreamEventEq(event, expectedEvent);
    return event._id;
}

function validateExpectedEventAndConfirmResumability(collParam, expectedOutput) {
    prepareCollection();

    let pipeline = [
        {$changeStream: {showExpandedEvents: true}},
        {$match: {operationType: {$nin: ["create", "createIndexes"]}}}
    ];

    let cursor = test.startWatchingChanges(
        {pipeline: pipeline, collection: collParam, aggregateOptions: {cursor: {batchSize: 0}}});

    assert.commandWorked(
        mongos.adminCommand({refineCollectionShardKey: kNsName, key: {_id: 1, akey: 1}}));

    // Confirm that we observe the refineCollectionShardKey event, and obtain its resume token.
    const refineResumeToken = assertExpectedEventObserved(cursor, expectedOutput);

    // Insert a document before starting the next change stream so that we can validate the
    // resuming behavior.
    assert.commandWorked(db[kCollName].insert({_id: numDocs + 1}));

    // Resume after the refine event and confirm we see the subsequent insert.
    pipeline = [{$changeStream: {showExpandedEvents: true, resumeAfter: refineResumeToken}}];
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

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, primaryShard);

// Test the behaviour of refineCollectionShardKey for a single-collection stream
validateExpectedEventAndConfirmResumability(kCollName, {
    operationType: "refineCollectionShardKey",
    ns: ns,
    operationDescription: {shardKey: {_id: 1, akey: 1}, oldShardKey: {_id: 1}}
});

// Test the behaviour of refineCollectionShardKey for a whole-DB stream.
validateExpectedEventAndConfirmResumability(1, {
    operationType: "refineCollectionShardKey",
    ns: ns,
    operationDescription: {shardKey: {_id: 1, akey: 1}, oldShardKey: {_id: 1}}
});

st.stop();
}());
