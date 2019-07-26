// Attempt to resume a change stream from the resume token for an "invalidate" event when the "drop"
// event that caused the invalidation is the last thing in the primary shard's oplog. There should
// be no error creating the new change stream, which should initially see no events. Reproduces the
// bug described in SERVER-41196.
// @tags: [requires_sharding, uses_change_streams]
(function() {
"use strict";

// The edge case we are testing occurs on an unsharded collection in a sharded cluster. We
// create a cluster with just one shard to ensure the test never blocks for another shard.
const st = new ShardingTest(
    {shards: 1, mongos: 1, rs: {nodes: 1, setParameter: {writePeriodicNoops: false}}});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

// Start a change stream that matches on the invalidate event.
const changeStream = mongosColl.watch([{'$match': {'operationType': 'invalidate'}}]);

// Create the collection by inserting into it and then drop the collection, thereby generating
// an invalidate event.
assert.commandWorked(mongosColl.insert({_id: 1}));
assert(mongosColl.drop());
assert.soon(() => changeStream.hasNext());
const invalidateEvent = changeStream.next();

// Resuming the change stream using the invalidate event allows us to see events after the drop.
const resumeStream = mongosColl.watch([], {startAfter: invalidateEvent["_id"]});

// The PBRT returned with the first (empty) batch should match the resume token we supplied.
assert.eq(bsonWoCompare(resumeStream.getResumeToken(), invalidateEvent["_id"]), 0);

// Initially, there should be no events visible after the drop.
assert(!resumeStream.hasNext());

// Add one last event and make sure the change stream sees it.
assert.commandWorked(mongosColl.insert({_id: 2}));
assert.soon(() => resumeStream.hasNext());
const afterDrop = resumeStream.next();
assert.eq(afterDrop.operationType, "insert");
assert.eq(afterDrop.fullDocument, {_id: 2});

st.stop();
})();
