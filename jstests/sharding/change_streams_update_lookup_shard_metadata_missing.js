/**
 * Tests that an updateLookup change stream doesn't throw ChangeStreamFatalError or
 * TooManyMatchingDocuments after fixing SERVER-44598.
 *
 * @tags: [
 *   uses_change_streams,
 * ]
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For ShardingTest.waitUntilStable.

// The UUID consistency check can hit NotPrimaryNoSecondaryOk when it attempts to obtain a list of
// collections from the shard Primaries through mongoS at the end of this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Start a new sharded cluster with 2 nodes and obtain references to the test DB and collection.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 3, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 5}}
});

let mongosDB = st.s.getDB(jsTestName());
let mongosColl = mongosDB.test;
let shard0 = st.rs0;

// Enable sharding on the the test database and ensure that the primary is shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), shard0.getURL());

// Shard the source collection on {a: 1}, split across the shards at {a: 0}.
st.shardColl(mongosColl, {a: 1}, {a: 0}, {a: 1});

// Open a change stream on the collection.
let csCursor = mongosColl.watch();

// Write one document onto shard0 and obtain its resume token.
assert.commandWorked(mongosColl.insert({_id: 0, a: -100}));
assert.soon(() => csCursor.hasNext());

const resumeToken = csCursor.next()._id;

// Obtain a reference to any secondary.
const newPrimary = shard0.getSecondary();

// Step up one of the Secondaries, which will not have any sharding metadata loaded.
shard0.stepUp(newPrimary);

// Make sure the mongoS and both shards sees shard0's new primary.
st.waitUntilStable();

// Refresh our reference to the test collection.
mongosColl = st.s.getDB(mongosDB.getName())[mongosColl.getName()];

// Do a {multi:true} update. This will scatter to all shards and update the document on shard0.
// Because no metadata is loaded, the shard will return a StaleShardVersion and fetch it, and
// the operation will be retried until it completes successfully.
assert.soonNoExcept(
    () => assert.commandWorked(mongosColl.update({_id: 0}, {$set: {updated: true}}, false, true)));

// Resume the change stream with {fullDocument: 'updateLookup'}. Update lookup can successfully
// identify the document based on its _id alone so long as the _id is unique in the collection, so
// this alone does not prove that the multi-update actually wrote its shard key into the oplog.
csCursor = mongosColl.watch([], {resumeAfter: resumeToken, fullDocument: "updateLookup"});
assert.soon(() => csCursor.hasNext());
assert.docEq({_id: 0, a: -100, updated: true}, csCursor.next().fullDocument);

// Now insert a new document with the same _id on the other shard. Update lookup will be able to
// distinguish between the two, proving that they both have full shard keys available.
assert.commandWorked(mongosColl.insert({_id: 0, a: 100}));
csCursor = mongosColl.watch([], {resumeAfter: resumeToken, fullDocument: "updateLookup"});
assert.soon(() => csCursor.hasNext());
assert.docEq({_id: 0, a: -100, updated: true}, csCursor.next().fullDocument);

st.stop();
})();
