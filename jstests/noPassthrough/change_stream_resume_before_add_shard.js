/**
 * Tests that a change stream can be resumed from a point in time before a new shard was added to
 * the cluster. Exercises the fix for SERVER-42232.
 * @tags: [uses_change_streams, requires_sharding]
 */
(function() {
"use strict";

const rsNodeOptions = {
    setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
};
const st =
    new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}, other: {rsOptions: rsNodeOptions}});

const mongosDB = st.s.getDB(jsTestName());
const coll = mongosDB.test;

// Helper function to confirm that a stream sees an expected sequence of documents. This
// function also pushes all observed changes into the supplied 'eventList' array.
function assertAllEventsObserved(changeStream, expectedDocs, eventList) {
    for (let expectedDoc of expectedDocs) {
        assert.soon(() => changeStream.hasNext());
        const nextEvent = changeStream.next();
        assert.eq(nextEvent.fullDocument, expectedDoc);
        if (eventList) {
            eventList.push(nextEvent);
        }
    }
}

// Helper function to add a new ReplSetTest shard into the cluster. Using single-node shards
// ensures that the "initiating set" entry cannot be rolled back.
function addShardToCluster(shardName) {
    const replTest = new ReplSetTest({name: shardName, nodes: 1, nodeOptions: rsNodeOptions});
    replTest.startSet({shardsvr: ""});
    replTest.initiate();
    assert.commandWorked(st.s.adminCommand({addShard: replTest.getURL(), name: shardName}));

    // Verify that the new shard's first oplog entry contains the string "initiating set". This
    // is used by change streams as a sentinel to indicate that no writes have occurred on the
    // replica set before this point.
    const firstOplogEntry = replTest.getPrimary().getCollection("local.oplog.rs").findOne();
    assert.docEq(firstOplogEntry.o, {msg: "initiating set"});
    assert.eq(firstOplogEntry.op, "n");

    return replTest;
}

// Helper function to resume from each event in a given list and confirm that the resumed stream
// sees the subsequent events in the correct expected order.
function assertCanResumeFromEachEvent(eventList) {
    for (let i = 0; i < eventList.length; ++i) {
        const resumedStream = coll.watch([], {resumeAfter: eventList[i]._id});
        for (let j = i + 1; j < eventList.length; ++j) {
            assert.soon(() => resumedStream.hasNext());
            assert.docEq(resumedStream.next(), eventList[j]);
        }
        resumedStream.close();
    }
}

// Open a change stream on the unsharded test collection.
const csCursor = coll.watch();
assert(!csCursor.hasNext());
const changeList = [];

// Insert some docs into the unsharded collection, and obtain a change stream event for each.
const insertedDocs = [{_id: 1}, {_id: 2}, {_id: 3}];
assert.commandWorked(coll.insert(insertedDocs));
assertAllEventsObserved(csCursor, insertedDocs, changeList);

// Verify that, for a brand new shard, we can start at an operation time before the set existed.
let startAtDawnOfTimeCursor = coll.watch([], {startAtOperationTime: Timestamp(1, 1)});
assertAllEventsObserved(startAtDawnOfTimeCursor, insertedDocs);
startAtDawnOfTimeCursor.close();

// Add a new shard into the cluster. Wait three seconds so that its initiation time is
// guaranteed to be later than any of the events in the existing shard's oplog.
const newShard1 = sleep(3000) || addShardToCluster("newShard1");

// .. and confirm that we can resume from any point before the shard was added.
assertCanResumeFromEachEvent(changeList);

// Now shard the collection on _id and move one chunk to the new shard.
st.shardColl(coll, {_id: 1}, {_id: 3}, false);
assert.commandWorked(st.s.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: 3}, to: "newShard1", _waitForDelete: true}));

// Insert some new documents into the new shard and verify that the original stream sees them.
const newInsertedDocs = [{_id: 4}, {_id: 5}];
assert.commandWorked(coll.insert(newInsertedDocs));
assertAllEventsObserved(csCursor, newInsertedDocs, changeList);

// Add a third shard into the cluster...
const newShard2 = sleep(3000) || addShardToCluster("newShard2");

// ... and verify that we can resume the stream from any of the preceding events.
assertCanResumeFromEachEvent(changeList);

// Now drop the collection, and verify that we can still resume from any point.
assert(coll.drop());
for (let expectedEvent of ["drop", "invalidate"]) {
    assert.soon(() => csCursor.hasNext());
    assert.eq(csCursor.next().operationType, expectedEvent);
}
assertCanResumeFromEachEvent(changeList);

// Verify that we can start at an operation time before the cluster existed and see all events.
startAtDawnOfTimeCursor = coll.watch([], {startAtOperationTime: Timestamp(1, 1)});
assertAllEventsObserved(startAtDawnOfTimeCursor, insertedDocs.concat(newInsertedDocs));
startAtDawnOfTimeCursor.close();

st.stop();

// Stop the new shards manually since the ShardingTest doesn't know anything about them.
newShard1.stopSet();
newShard2.stopSet();
})();
