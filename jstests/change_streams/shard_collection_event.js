/**
 * Test that change streams returns shardCollection events with the options specified on the
 * original user command.
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

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.

jsTestLog("creating sharding test");
var st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const dbName = jsTestName();
const collName = "test";
const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

const ns = {
    db: dbName,
    coll: collName
};

const collNS = dbName + "." + collName;

function getCollectionUuid(coll) {
    const collInfo = db.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

function assertNextChangeEvent(cursor, expectedEvent) {
    let events = test.getNextChanges(cursor, 1);
    while (events.length > 0) {
        jsTestLog("got event " + tojson(events[0]));
        if (events[0].operationType == "shardCollection") {
            break;
        }
        // The only possible other events are create collection or index.
        assert(events[0].operationType == "create" || events[0].operationType == "createIndexes");
        events = test.getNextChanges(cursor, 1);
    }

    let event = events[0];
    // Check the presence and the type of 'wallTime' field. We have no way to check the correctness
    // of 'wallTime' value, so we delete it afterwards.
    assert(event.wallTime instanceof Date);
    delete event.wallTime;
    expectedEvent.collectionUUID = getCollectionUuid(collName);
    assertChangeStreamEventEq(event, expectedEvent);
}

function runTest(startChangeStream) {
    function validateExpectedEventAndDropCollection(command, expectedOutput) {
        assertDropCollection(db, collName);
        const cursor = startChangeStream();
        assert.commandWorked(db.adminCommand(command));
        assertNextChangeEvent(cursor, expectedOutput);
    }

    function validateExpectedEventAndConfirmResumability(command, expectedOutput) {
        assertDropAndRecreateCollection(db, collName);
        let pipeline = [{$changeStream: {showExpandedEvents: true}}];
        let cursor = test.startWatchingChanges(
            {pipeline, collection: collName, aggregateOptions: {cursor: {batchSize: 0}}});

        assert.commandWorked(db.adminCommand(command));

        let events = test.getNextChanges(cursor, 1);
        while (events.length > 0) {
            jsTestLog("got event " + tojson(events[0]));
            if (events[0].operationType == "shardCollection") {
                break;
            }
            // The only possible other events are create collection or index.
            assert(events[0].operationType == "create" ||
                   events[0].operationType == "createIndexes");
            events = test.getNextChanges(cursor, 1);
        }

        const shardEvent = events[0];
        assertChangeStreamEventEq(shardEvent, expectedOutput);

        // Insert a document before starting the next change stream so that we can validate the
        // resuming behavior.
        assert.commandWorked(db[collName].insert({_id: 1}));

        // Resume with 'resumeAfter'.
        pipeline = [{$changeStream: {showExpandedEvents: true, resumeAfter: shardEvent._id}}];
        cursor = test.startWatchingChanges({pipeline: pipeline, collection: collName});

        test.assertNextChangesEqual({
            cursor,
            expectedChanges: {
                operationType: "insert",
                ns: ns,
                fullDocument: {_id: 1},
                documentKey: {_id: 1},
            }
        });
    }

    validateExpectedEventAndConfirmResumability({shardCollection: collNS, key: {_id: 1}}, {
        operationType: "shardCollection",
        ns: ns,
        operationDescription: {
            "shardKey": {"_id": 1},
            "unique": false,
            "numInitialChunks": NumberLong(0),
            "presplitHashedZones": false
        }
    });

    validateExpectedEventAndConfirmResumability({shardCollection: collNS, key: {_id: "hashed"}}, {
        operationType: "shardCollection",
        ns: ns,
        operationDescription: {
            "shardKey": {"_id": "hashed"},
            "unique": false,
            "numInitialChunks": NumberLong(0),
            "presplitHashedZones": false
        }
    });

    /* This test verifies simple key parameter passing .*/
    validateExpectedEventAndDropCollection({shardCollection: collNS, key: {_id: 1}}, {
        operationType: "shardCollection",
        ns: ns,
        operationDescription: {
            "shardKey": {"_id": 1},
            "unique": false,
            "numInitialChunks": NumberLong(0),
            "presplitHashedZones": false
        }
    });

    /* This test verifies simple hashed key parameter passing .*/
    validateExpectedEventAndDropCollection({shardCollection: collNS, key: {_id: "hashed"}}, {
        operationType: "shardCollection",
        ns: ns,
        operationDescription: {
            "shardKey": {"_id": "hashed"},
            "unique": false,
            "numInitialChunks": NumberLong(0),
            "presplitHashedZones": false
        }
    });

    /* This test verifies compound hashed key parameter passing .*/
    validateExpectedEventAndDropCollection({shardCollection: collNS, key: {x: "hashed", y: 1}}, {
        operationType: "shardCollection",
        ns: ns,
        operationDescription: {
            "shardKey": {"x": "hashed", "y": 1},
            "unique": false,
            "numInitialChunks": NumberLong(0),
            "presplitHashedZones": false
        }
    });

    /* This test verifies numInitialChunks parameter passing .*/
    validateExpectedEventAndDropCollection(
        {shardCollection: collNS, key: {_id: "hashed"}, numInitialChunks: 10}, {
            operationType: "shardCollection",
            ns: ns,
            operationDescription: {
                "shardKey": {"_id": "hashed"},
                "unique": false,
                "numInitialChunks": NumberLong(10),
                "presplitHashedZones": false
            }
        });

    /* This test verifies unique parameter passing .*/
    validateExpectedEventAndDropCollection({shardCollection: collNS, key: {_id: 1}, unique: true}, {
        operationType: "shardCollection",
        ns: ns,
        operationDescription: {
            "shardKey": {"_id": 1},
            "unique": true,
            "numInitialChunks": NumberLong(0),
            "presplitHashedZones": false
        }
    });

    /* This test verifies collation parameter passing .*/
    validateExpectedEventAndDropCollection(
        {shardCollection: collNS, key: {_id: 1}, collation: {locale: 'simple'}}, {
            operationType: "shardCollection",
            ns: ns,
            operationDescription: {
                "shardKey": {"_id": 1},
                "unique": false,
                "numInitialChunks": NumberLong(0),
                "presplitHashedZones": false,
                "collation": {"locale": "simple"}
            }
        });
}

assert.commandWorked(db.adminCommand({enableSharding: dbName}));
const test = new ChangeStreamTest(db);
const pipeline = [{$changeStream: {showExpandedEvents: true}}];
runTest(() => test.startWatchingChanges({pipeline, collection: 1}));
runTest(() => test.startWatchingChanges({pipeline, collection: collName}));
st.stop();
}());
