// Tests metadata notifications of change streams on sharded collections.
// @tags: [
//   requires_majority_read_concern,
// ]
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 1,
        enableMajorityReadConcern: '',
    }
});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

assert.commandWorked(mongosDB.dropDatabase());

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

// Shard the test collection on a field called 'shardKey'.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {shardKey: 1}}));

// Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
assert.commandWorked(
    mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {shardKey: 0}}));

// Move the [0, MaxKey] chunk to st.shard1.shardName.
assert.commandWorked(mongosDB.adminCommand(
    {moveChunk: mongosColl.getFullName(), find: {shardKey: 1}, to: st.rs1.getURL()}));

// Write a document to each chunk.
assert.commandWorked(mongosColl.insert({shardKey: -1, _id: -1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(mongosColl.insert({shardKey: 1, _id: 1}, {writeConcern: {w: "majority"}}));

let changeStream = mongosColl.watch();

// We awaited the replication of the first writes, so the change stream shouldn't return them.
assert.commandWorked(mongosColl.update({shardKey: -1, _id: -1}, {$set: {updated: true}}));
assert.commandWorked(mongosColl.update({shardKey: 1, _id: 1}, {$set: {updated: true}}));
assert.commandWorked(mongosColl.insert({shardKey: 2, _id: 2}));

// Drop the collection and test that we return a "drop" entry, followed by an "invalidate"
// entry.
mongosColl.drop();

// Test that we see the two writes that happened before the collection drop.
assert.soon(() => changeStream.hasNext());
let next = changeStream.next();
assert.eq(next.operationType, "update");
assert.eq(next.documentKey.shardKey, -1);
const resumeTokenFromFirstUpdate = next._id;

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "update");
assert.eq(next.documentKey.shardKey, 1);

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "insert");
assert.eq(next.documentKey._id, 2);

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "drop");
assert.eq(next.ns, {db: mongosDB.getName(), coll: mongosColl.getName()});

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "invalidate");

// Store the collection drop invalidate token for for subsequent tests.
const collectionDropinvalidateToken = next._id;

assert(!changeStream.hasNext());
assert(changeStream.isExhausted());

// Verify that even after filtering out all events, the cursor still returns the invalidate resume
// token of the dropped collection.
const resumeStream = mongosColl.watch([{$match: {operationType: "DummyOperationType"}}],
                                      {resumeAfter: resumeTokenFromFirstUpdate});
assert.soon(() => {
    assert(!resumeStream.hasNext());
    return resumeStream.isExhausted();
});
assert.eq(resumeStream.getResumeToken(), collectionDropinvalidateToken);

// With an explicit collation, test that we can resume from before the collection drop.
changeStream =
    mongosColl.watch([], {resumeAfter: resumeTokenFromFirstUpdate, collation: {locale: "simple"}});

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "update");
assert.eq(next.documentKey, {shardKey: 1, _id: 1});

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "insert");
assert.eq(next.documentKey, {shardKey: 2, _id: 2});

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "drop");
assert.eq(next.ns, {db: mongosDB.getName(), coll: mongosColl.getName()});

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "invalidate");
assert(!changeStream.hasNext());
assert(changeStream.isExhausted());

// Test that we can resume the change stream without specifying an explicit collation.
assert.commandWorked(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
    cursor: {}
}));

// Recreate and shard the collection.
assert.commandWorked(mongosDB.createCollection(mongosColl.getName()));

// Shard the test collection on shardKey.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {shardKey: 1}}));

// Test that resuming the change stream on the recreated collection succeeds, since we will not
// attempt to inherit the collection's default collation and can therefore ignore the new UUID.
assert.commandWorked(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
    cursor: {}
}));

// Recreate the collection as unsharded and open a change stream on it.
assertDropAndRecreateCollection(mongosDB, mongosColl.getName());

changeStream = mongosColl.watch();

// Drop the database and verify that the stream returns a collection drop followed by an
// invalidate.
assert.commandWorked(mongosDB.dropDatabase());

assert.soon(() => changeStream.hasNext());
next = changeStream.next();

// Store the token to be used as 'resumeAfter' token by other change streams.
const resumeTokenAfterDbDrop = next._id;

assert.eq(next.operationType, "drop");
assert.eq(next.ns, {db: mongosDB.getName(), coll: mongosColl.getName()});

assert.soon(() => changeStream.hasNext());
next = changeStream.next();
assert.eq(next.operationType, "invalidate");
assert(!changeStream.hasNext());
assert(changeStream.isExhausted());

// Store the database drop invalidate token for other change streams.
const dbDropInvalidateToken = next._id;

// Verify that even after filtering out all events, the cursor still returns the invalidate resume
// token of the dropped database.
const resumeStream1 = mongosColl.watch([{$match: {operationType: "DummyOperationType"}}],
                                       {resumeAfter: resumeTokenAfterDbDrop});
assert.soon(() => {
    assert(!resumeStream1.hasNext());
    return resumeStream1.isExhausted();
});
assert.eq(resumeStream1.getResumeToken(), dbDropInvalidateToken);

st.stop();
})();
