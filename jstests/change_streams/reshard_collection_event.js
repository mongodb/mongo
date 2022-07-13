/**
 * Test that change streams returns reshardCollection events.
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
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000}}
    }
});

const mongos = st.s0;
const primaryShard = st.shard0.shardName;
const kDbName = jsTestName();
const kCollName = 'coll';
const kNsName = kDbName + '.' + kCollName;
const numDocs = 1;
const zoneName = 'zone1';

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
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: zoneName}));
}

function assertExpectedEventObserved(cursor, expectedEvent) {
    let events = test.getNextChanges(cursor, 1);
    let event = events[0];
    // Check the presence and the type of 'wallTime' field. We have no way to check the correctness
    // of 'wallTime' value, so we delete it afterwards.
    assert(event.wallTime instanceof Date);
    delete event.wallTime;
    assertChangeStreamEventEq(event, expectedEvent);
    return event._id;
}

function validateExpectedEventAndConfirmResumability(collParam, expectedOutput) {
    prepareCollection();
    let collectionUUID = getCollectionUuid(kCollName);

    let pipeline = [
        {$changeStream: {showExpandedEvents: true}},
        {$match: {operationType: {$nin: ["create", "createIndexes"]}}}
    ];

    let cursor = test.startWatchingChanges(
        {pipeline: pipeline, collection: collParam, aggregateOptions: {cursor: {batchSize: 0}}});

    assert.commandWorked(mongos.adminCommand({
        reshardCollection: kNsName,
        key: {newKey: 1},
        unique: false,
        numInitialChunks: 1,
        collation: {locale: 'simple'},
        zones: [{zone: zoneName, min: {newKey: MinKey}, max: {newKey: MaxKey}}]
    }));

    // Confirm that we observe the reshardCollection event, and obtain its resume token.
    expectedOutput.collectionUUID = collectionUUID;
    expectedOutput.operationDescription.reshardUUID = getCollectionUuid(kCollName);
    const reshardResumeToken = assertExpectedEventObserved(cursor, expectedOutput);

    // Insert a document before starting the next change stream so that we can validate the
    // resuming behavior.
    assert.commandWorked(db[kCollName].insert({_id: numDocs + 1}));

    // Resume after the reshard event and confirm we see the subsequent insert.
    pipeline = [{$changeStream: {showExpandedEvents: true, resumeAfter: reshardResumeToken}}];
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

// Test the behaviour of reshardCollection for a collection.
// The values of 'collectionUUID' and 'reshardUUID' will be filled in by the validate function.
validateExpectedEventAndConfirmResumability(kCollName, {
    "operationType": "reshardCollection",
    "collectionUUID": 0,
    "ns": {"db": "reshard_collection_event", "coll": "coll"},
    "operationDescription": {
        "reshardUUID": 0,
        "shardKey": {"newKey": 1},
        "oldShardKey": {"_id": 1},
        "unique": false,
        "numInitialChunks": NumberLong(1),
        "collation": {"locale": "simple"},
        "zones": [
            {"zone": "zone1", "min": {"newKey": {"$minKey": 1}}, "max": {"newKey": {"$maxKey": 1}}}
        ]
    }
});

// Test the behaviour of reshardCollection for a whole-DB stream.
// The values of 'collectionUUID' and 'reshardUUID' will be filled in by the validate function.
validateExpectedEventAndConfirmResumability(1, {
    "operationType": "reshardCollection",
    "collectionUUID": 0,
    "ns": {"db": "reshard_collection_event", "coll": "coll"},
    "operationDescription": {
        "reshardUUID": 0,
        "shardKey": {"newKey": 1},
        "oldShardKey": {"_id": 1},
        "unique": false,
        "numInitialChunks": NumberLong(1),
        "collation": {"locale": "simple"},
        "zones": [
            {"zone": "zone1", "min": {"newKey": {"$minKey": 1}}, "max": {"newKey": {"$maxKey": 1}}}
        ]
    }
});

st.stop();
}());
