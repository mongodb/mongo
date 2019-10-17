/**
 * Tests that existing whole-cluster, whole-db and single-collection $changeStreams correctly pick
 * up events on a newly-added shard when a new unsharded collection is created on it. Exercises the
 * fix for SERVER-42723.
 * Tagging as 'requires_find_command' to ensure that this test is not run in the legacy protocol
 * passthroughs. Legacy getMore fails in cases where it is run on a database or collection which
 * does not yet exist.
 * @tags: [uses_change_streams, requires_sharding, requires_find_command]
 */
(function() {

"use strict";

const rsNodeOptions = {
    setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
};
const st =
    new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}, other: {rsOptions: rsNodeOptions}});

// We require one 'test' database and a second 'other' database.
const oldShardDB = st.s.getDB(jsTestName() + "_other");
const newShardDB = st.s.getDB(jsTestName());

const configDB = st.s.getDB("config");
const adminDB = st.s.getDB("admin");

const oldShardColl = oldShardDB.coll;
const newShardColl = newShardDB.test;

// Helper function to add a new ReplSetTest shard into the cluster.
function addShardToCluster(shardName) {
    const replTest = new ReplSetTest({name: shardName, nodes: 1, nodeOptions: rsNodeOptions});
    replTest.startSet({shardsvr: ""});
    replTest.initiate();
    assert.commandWorked(st.s.adminCommand({addShard: replTest.getURL(), name: shardName}));
    return replTest;
}

// Helper function to confirm that a stream sees an expected sequence of documents.
function assertAllEventsObserved(changeStream, expectedDocs) {
    for (let expectedDoc of expectedDocs) {
        assert.soon(() => changeStream.hasNext());
        const nextEvent = changeStream.next();
        assert.docEq(nextEvent.fullDocument, expectedDoc);
    }
}

// Open a whole-db change stream on the as yet non-existent database.
const wholeDBCS = newShardDB.watch();

// Open a single-collection change stream on a namespace within the non-existent database.
const singleCollCS = newShardColl.watch();

// Open a whole-cluster stream on the deployment.
const wholeClusterCS = adminDB.aggregate([{$changeStream: {allChangesForCluster: true}}]);

// Insert some data into the 'other' database on the only existing shard. This should ensure that
// the primary shard of the test database will be created on the second shard, after it is added.
const insertedDocs = Array.from({length: 20}, (_, i) => ({_id: i}));
assert.commandWorked(oldShardColl.insert(insertedDocs));

// Verify that the whole-cluster stream sees all these events.
assertAllEventsObserved(wholeClusterCS, insertedDocs);

// Verify that the other two streams did not see any of the insertions on the 'other' collection.
for (let csCursor of [wholeDBCS, singleCollCS]) {
    assert(!csCursor.hasNext());
}

// Now add a new shard into the cluster...
const newShard1 = addShardToCluster("newShard1");

// ... create a new database and collection, and verify that they were placed on the new shard....
assert.commandWorked(newShardDB.runCommand({create: newShardColl.getName()}));
assert(configDB.databases.findOne({_id: newShardDB.getName(), primary: "newShard1"}));

// ... insert some documents into the new, unsharded collection on the new shard...
assert.commandWorked(newShardColl.insert(insertedDocs));

// ... and confirm that all the pre-existing streams see all of these events.
for (let csCursor of [singleCollCS, wholeDBCS, wholeClusterCS]) {
    assertAllEventsObserved(csCursor, insertedDocs);
}

// Stop the new shard manually since the ShardingTest doesn't know anything about it.
st.stop();
newShard1.stopSet();
})();
