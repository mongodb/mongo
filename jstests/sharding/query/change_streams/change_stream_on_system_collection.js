/**
 * Test for $changeStream on system.* namespaces using internal allowToRunOnSystemNS parameter in a
 * pattern used by the resharding processes where an event in an oplog triggers a start of an
 * observation of a newly created system.* collection.
 *
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   requires_majority_read_concern,
 *   uses_change_streams,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {assertChangeStreamEventEq} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test involves opening change stream on the internal collection, which is not allowed through
// a router.
TestData.replicaSetEndpointIncompatible = true;

// Asserts that the next event in a change stream connected to via cursor 'changeStreamCursor' is
// equal to 'eventDocument'.
function assertNextChangeStreamEventEquals(changeStreamCursor, eventDocument) {
    assert.soon(() => changeStreamCursor.hasNext());
    assertChangeStreamEventEq(changeStreamCursor.next(), eventDocument);
}

const st = new ShardingTest({
    shards: 1,
    rs: {
        // Use the noop writer with a higher frequency for periodic noops to speed up the test.
        setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
    },
});
const db = st.s.getDB(jsTestName());
const dbName = db.getName();

// Enable sharding on the test DB.
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

// Drop and recreate the collections to be used in this set of tests.
assertDropAndRecreateCollection(db, "t1");

// Watch all database-wide events to capture the cluster time of the first operation afer the
// creation of the collection system.* that we will watch later and to check that system.*
// collection events are not observed in the db-wide change stream.
const wholeDBCursor = db.watch();

// Watch all cluster events. We will later use this to demonstrate that writes to a system
// collection do not show up in a cluster-wide change stream.
const wholeClusterCursor = db.getMongo().watch();

// Create a new sharded collection that we will start watching later.
assert.commandWorked(db.createCollection("system.resharding.someUUID"));
assert.commandWorked(db.adminCommand({shardCollection: db.getName() + ".system.resharding.someUUID", key: {_id: 1}}));

// Insert a document to capture the cluster time at which our tests begin.
assert.commandWorked(db.t1.insert({_id: 0, a: 1}));
assert.soon(() => wholeDBCursor.hasNext());
const documentInsertedEvent = wholeDBCursor.next();

// Verify that the event is a document insertion event.
assert.eq("insert", documentInsertedEvent.operationType, "Unexpected change event: " + tojson(documentInsertedEvent));
assert.eq("t1", documentInsertedEvent.ns.coll, "Unexpected change event: " + tojson(documentInsertedEvent));
const clusterTimeAtInsert = documentInsertedEvent.clusterTime;

const systemCollection = db["system.resharding.someUUID"];

// Insert a document into a system.* collection. We will open a stream to observe this event later
// in the test.
assert.commandWorked(systemCollection.insert({_id: 1, a: 1}));

// Insert one more document to advance cluster time.
assert.commandWorked(db.t1.insert({_id: 2, a: 1}));

// Verify that the system rejects a request to open a change stream on a system.* collection through
// a mongos process even if parameter allowToRunOnSystemNS=true.
assert.throwsWithCode(
    () => systemCollection.watch([], {allowToRunOnSystemNS: true}),
    ErrorCodes.InvalidNamespace,
    [],
    "expected a request with 'allowToRunOnSystemNS: true' to open a change stream on a system collection through mongos to fail",
);

const systemCollectionThroughShard = st.shard0.getCollection(systemCollection.getFullName());

// Start watching system.* collection by opening a change stream through a shard.
const systemCollectionThroughShardCursor = systemCollectionThroughShard.watch([], {
    startAtOperationTime: clusterTimeAtInsert,
    allowToRunOnSystemNS: true,
});

// Verify that a document insert event in a system.* collection is observed.
assertNextChangeStreamEventEquals(systemCollectionThroughShardCursor, {
    documentKey: {_id: 1},
    fullDocument: {_id: 1, a: 1},
    ns: {db: dbName, coll: "system.resharding.someUUID"},
    operationType: "insert",
});

// Verify that the document insertion into system.resharding.someUUID event was not observed in a
// db-wide change stream.
assert.commandWorked(db.t1.insert({_id: 3, a: 1}));
assertNextChangeStreamEventEquals(wholeDBCursor, {
    documentKey: {_id: 2},
    fullDocument: {_id: 2, a: 1},
    ns: {db: dbName, coll: "t1"},
    operationType: "insert",
});
assertNextChangeStreamEventEquals(wholeDBCursor, {
    documentKey: {_id: 3},
    fullDocument: {_id: 3, a: 1},
    ns: {db: dbName, coll: "t1"},
    operationType: "insert",
});

// Verify that the document insertion into system.resharding.someUUID event was not observed in a
// cluster-wide change stream.
assertNextChangeStreamEventEquals(wholeClusterCursor, {
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 1},
    ns: {db: dbName, coll: "t1"},
    operationType: "insert",
});
assertNextChangeStreamEventEquals(wholeClusterCursor, {
    documentKey: {_id: 2},
    fullDocument: {_id: 2, a: 1},
    ns: {db: dbName, coll: "t1"},
    operationType: "insert",
});
assertNextChangeStreamEventEquals(wholeClusterCursor, {
    documentKey: {_id: 3},
    fullDocument: {_id: 3, a: 1},
    ns: {db: dbName, coll: "t1"},
    operationType: "insert",
});

// The temporary reshard collection must be dropped before checking metadata integrity.
assert(systemCollection.drop());

st.stop();
