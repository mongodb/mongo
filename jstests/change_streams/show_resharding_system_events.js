/**
 * Tests the behavior of change streams on a system.resharding.* namespace in the presence of
 * 'showSystemEvents' flag. This is a separate test from 'show_system_events.js' because it can only
 * operate in a sharded cluster.
 *
 * @tags: [
 *  requires_fcv_61,
 *  featureFlagChangeStreamsFurtherEnrichedEvents,
 *  requires_sharding,
 *  uses_change_streams,
 *  change_stream_does_not_expect_txns,
 *  assumes_unsharded_collection,
 *  assumes_read_preference_unchanged,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/change_stream_util.js');  // For 'assertChangeStreamEventEq'.

// Create a single-shard cluster for this test.
const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000}}
    }
});

const testDB = st.s.getDB(jsTestName());
const testColl = testDB[jsTestName()];

// Shard the collection based on '_id'.
st.shardColl(testColl, {_id: 1}, false);

// Build an index on the collection to support the resharding operation.
assert.commandWorked(testColl.createIndex({a: 1}));

// Insert some documents that will be resharded.
assert.commandWorked(testColl.insert({_id: 0, a: 0}));
assert.commandWorked(testColl.insert({_id: 1, a: 1}));

// Helper function to retrieve the UUID of the specified collection.
function getCollectionUuid(coll) {
    const collInfo = testDB.getCollectionInfos({name: coll.getName()})[0];
    return collInfo.info.uuid;
}

// Obtain a resume token indicating the start point for the test.
const startPoint = testDB.watch().getResumeToken();

// Get the UUID of the collection before resharding.
const oldUUID = getCollectionUuid(testColl);

// Reshard the collection.
assert.commandWorked(st.s.adminCommand({
    reshardCollection: testColl.getFullName(),
    key: {a: 1},
}));

// Get the UUID of the collection after resharding.
const newUUID = getCollectionUuid(testColl);

// Write one more sentinel document into the collection.
assert.commandWorked(testColl.insert({_id: 2, a: 2}));

// Now confirm the sequence of events that we expect to see in the change stream.
const reshardingCollName = `system.resharding.${oldUUID.toString().match(/\"([^\"]+)\"/)[1]}`;
const reshardingNs = {
    db: testDB.getName(),
    coll: reshardingCollName
};
const origNs = {
    db: testDB.getName(),
    coll: testColl.getName()
};
const expectedReshardingEvents = [
    {ns: reshardingNs, collectionUUID: newUUID, operationType: "create"},
    {
        ns: reshardingNs,
        collectionUUID: newUUID,
        operationType: "createIndexes",
        operationDescription: {indexes: [{v: 2, key: {a: 1}, name: "a_1"}]}
    },
    {
        ns: reshardingNs,
        collectionUUID: newUUID,
        operationType: "shardCollection",
        operationDescription: {shardKey: {a: 1}}
    },
    {
        ns: reshardingNs,
        collectionUUID: newUUID,
        operationType: "insert",
        fullDocument: {_id: 0, a: 0},
        documentKey: {a: 0, _id: 0}
    },
    {
        ns: reshardingNs,
        collectionUUID: newUUID,
        operationType: "insert",
        fullDocument: {_id: 1, a: 1},
        documentKey: {a: 1, _id: 1}
    },
    {
        ns: origNs,
        collectionUUID: oldUUID,
        operationType: "reshardCollection",
        operationDescription:
            {reshardUUID: newUUID, shardKey: {a: 1}, oldShardKey: {_id: 1}, unique: false}
    },
    {
        ns: origNs,
        collectionUUID: newUUID,
        operationType: "insert",
        fullDocument: {_id: 2, a: 2},
        documentKey: {a: 2, _id: 2}
    },
];

// Helper to confirm the sequence of events observed in the change stream.
function assertChangeStreamEventSequence(csConfig, expectedEvents) {
    // Open a change stream on the test DB using the given configuration.
    const finalConfig =
        Object.assign({resumeAfter: startPoint, showExpandedEvents: true}, csConfig);
    const csCursor = testDB.watch([], finalConfig);

    // Confirm that we see the expected sequence of events.
    expectedEvents.forEach((expectedEvent) => {
        assert.soon(() => csCursor.hasNext());
        assertChangeStreamEventEq(csCursor.next(), expectedEvent);
    });
}

// With showSystemEvents set to true, we expect to see the full sequence of events.
assertChangeStreamEventSequence({showSystemEvents: true}, expectedReshardingEvents);

// With showSystemEvents set to false, we expect to only see events on the original namespace.
const nonSystemEvents =
    expectedReshardingEvents.filter((event) => (event.ns.coll === testColl.getName()));
assertChangeStreamEventSequence({showSystemEvents: false}, nonSystemEvents);

st.stop();
}());
