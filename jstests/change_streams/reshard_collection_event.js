/**
 * Test that change streams return the expected reshardCollection event for each related end-user
 * command (reshardCollection, moveCollection, unshardCollection).
 *
 *  @tags: [
 *    requires_sharding,
 *    uses_change_streams,
 *    change_stream_does_not_expect_txns,
 *    assumes_unsharded_collection,
 *    assumes_read_preference_unchanged,
 * ]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventEq,
    ChangeStreamTest
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000}}
    }
});

const mongos = st.s0;
const primaryShard = st.shard0.shardName;
const nonPrimaryShard = st.shard1.shardName;
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

function prepareShardedCollection() {
    assertDropCollection(db, kCollName);
    assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: zoneName}));
}

function prepareUnshardedCollection() {
    assertDropCollection(db, kCollName);
    assert.commandWorked(db.createCollection(kCollName));
}

function reshardCollectionCmd() {
    assert.commandWorked(mongos.adminCommand({
        reshardCollection: kNsName,
        key: {newKey: 1},
        unique: false,
        numInitialChunks: 1,
        collation: {locale: 'simple'},
        zones: [{zone: zoneName, min: {newKey: MinKey}, max: {newKey: MaxKey}}]
    }));
}

function moveCollectionCmd() {
    assert.commandWorked(mongos.adminCommand({moveCollection: kNsName, toShard: nonPrimaryShard}));
}

function unshardCollectionCmd() {
    assert.commandWorked(
        mongos.adminCommand({unshardCollection: kNsName, toShard: nonPrimaryShard}));
}

function assertExpectedEventObserved(cursor, expectedEvent) {
    let events = test.getNextChanges(cursor, 1);
    let event = events[0];
    // Check the presence and the type of 'wallTime' field. We have no way to check the
    // correctness of 'wallTime' value, so we delete it afterwards.
    assert(event.wallTime instanceof Date);
    delete event.wallTime;
    assertChangeStreamEventEq(event, expectedEvent);
    return event._id;
}

function validateExpectedEventAndConfirmResumability(
    setupFn, userCmdFn, collParam, expectedOutput) {
    setupFn();
    let collectionUUID = getCollectionUuid(kCollName);

    let pipeline = [
        {$changeStream: {showExpandedEvents: true}},
        {$match: {operationType: {$nin: ["create", "createIndexes"]}}}
    ];

    let cursor = test.startWatchingChanges(
        {pipeline: pipeline, collection: collParam, aggregateOptions: {cursor: {batchSize: 0}}});

    userCmdFn();

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

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: primaryShard}));

for (let watchCollectionParameter of [kCollName, 1]) {
    const watchLevel = watchCollectionParameter === 1 ? kDbName : watchCollectionParameter;
    jsTest.log(`Validate behavior of reshardCollection against a change stream watching at '${
        watchLevel}' level`);

    // The values of 'collectionUUID' and 'reshardUUID' will be filled in by the validate function.
    validateExpectedEventAndConfirmResumability(
        prepareShardedCollection, reshardCollectionCmd, watchCollectionParameter, {
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
                "provenance": "reshardCollection",
                "zones": [{
                    "zone": "zone1",
                    "min": {"newKey": {"$minKey": 1}},
                    "max": {"newKey": {"$maxKey": 1}}
                }]
            }
        });

    jsTest.log(`Validate behavior of moveCollection against a change stream watching at '${
        watchLevel}' level`);
    validateExpectedEventAndConfirmResumability(
        prepareUnshardedCollection, moveCollectionCmd, watchCollectionParameter, {
            "operationType": "reshardCollection",
            "collectionUUID": 0,
            "ns": {"db": "reshard_collection_event", "coll": "coll"},
            "operationDescription": {
                "reshardUUID": 0,
                "shardKey": {"_id": 1},
                "oldShardKey": {"_id": 1},
                "unique": false,
                "numInitialChunks": NumberLong(1),
                "provenance": "moveCollection",
            }
        });

    jsTest.log(`Validate behavior of unshardCollection against a change stream watching at '${
        watchLevel}' level`);
    validateExpectedEventAndConfirmResumability(
        prepareShardedCollection, unshardCollectionCmd, watchCollectionParameter, {
            "operationType": "reshardCollection",
            "collectionUUID": 0,
            "ns": {"db": "reshard_collection_event", "coll": "coll"},
            "operationDescription": {
                "reshardUUID": 0,
                "shardKey": {"_id": 1},
                "oldShardKey": {"_id": 1},
                "unique": false,
                "numInitialChunks": NumberLong(1),
                "provenance": "unshardCollection",
            }
        });
}

st.stop();
