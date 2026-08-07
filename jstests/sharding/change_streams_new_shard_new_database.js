/**
 * Tests that a $changeStream correctly opens a cursor on a newly-added shard and delivers its
 * events, regardless of how the stream was resumed (resumeAfter, startAfter,
 * startAtOperationTime) or at what scope it was opened (single-collection, whole-db,
 * whole-cluster). Exercises the fix for SERVER-42723.
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 *   multiversion_incompatible,
 * ]
 */
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

const rsNodeOptions = {
    setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
};

let st;
let configDB;
let adminDB;

function setup() {
    st = new ShardingTest({
        shards: 1,
        mongos: 1,
        rs: {nodes: 1},
        other: {rsOptions: rsNodeOptions}
    });
    configDB = st.s.getDB("config");
    adminDB = st.s.getDB("admin");
}

function tearDown() {
    st.stop();
}

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
        assert.docEq(expectedDoc, nextEvent.fullDocument, tojson(nextEvent));
    }
}

// Produces a resume token representing "now", for building a 'resumeAfter'/'startAfter'
// 'changeStreamSpec' argument to testChangeStreamWillOpenCursorsOnNewShardCorrectly() below.
function getResumeTokenNow() {
    const cursor = adminDB.aggregate([{$changeStream: {allChangesForCluster: true}}]);
    const resumeToken = cursor.getResumeToken();
    cursor.close();
    return resumeToken;
}

// Produces an operation time representing "now", for building a 'startAtOperationTime'
// 'changeStreamSpec' argument to testChangeStreamWillOpenCursorsOnNewShardCorrectly() below.
function getOperationTimeNow() {
    return assert.commandWorked(adminDB.runCommand({ping: 1})).operationTime;
}

// Tests that existing whole-cluster, whole-db and single-collection $changeStreams, opened via
// 'changeStreamSpec', correctly pick up events on a newly-added shard when a new unsharded
// collection is created on it.
function testChangeStreamWillOpenCursorsOnNewShardCorrectly(changeStreamSpec) {
    // We require one 'test' database and a second 'other' database.
    const oldShardDB = st.s.getDB(jsTestName() + "_other");
    const newShardDB = st.s.getDB(jsTestName());
    const oldShardColl = oldShardDB.coll;
    const newShardColl = newShardDB.test;

    // Open a whole-db change stream on the as yet non-existent database.
    const wholeDBCS = newShardDB.watch([], changeStreamSpec);

    // Open a single-collection change stream on a namespace within the non-existent database.
    const singleCollCS = newShardColl.watch([], changeStreamSpec);

    // Open a whole-cluster stream on the deployment.
    const wholeClusterCS = adminDB.aggregate(
        [{$changeStream: Object.assign({allChangesForCluster: true}, changeStreamSpec)}]);

    // Insert some data into the 'other' database on the only existing shard. This should ensure
    // that the primary shard of the test database will be created on the second shard, after it
    // is added.
    const insertedDocs = Array.from({length: 20}, (_, i) => ({_id: i}));
    assert.commandWorked(oldShardColl.insert(insertedDocs));

    // Verify that the whole-cluster stream sees all these events.
    assertAllEventsObserved(wholeClusterCS, insertedDocs);

    // Verify that the other two streams did not see any of the insertions on the 'other'
    // collection.
    for (let csCursor of [wholeDBCS, singleCollCS]) {
        assert(!csCursor.hasNext());
    }

    // Now add a new shard into the cluster...
    const newShard1 = addShardToCluster("newShard1");

    // .. make sure the primary shard of 'newShardDB' database is the new shard ..
    assert.commandWorked(
        st.s.adminCommand({enableSharding: newShardDB.getName(), primaryShard: "newShard1"}));
    assert.neq(configDB.databases.findOne({_id: newShardDB.getName(), primary: "newShard1"}),
               null);

    //... create a new collection, and verify that it was placed on the new shard....
    assert.commandWorked(newShardDB.runCommand({create: newShardColl.getName()}));
    assert(configDB.databases.findOne({_id: newShardDB.getName(), primary: "newShard1"}));

    // ... insert some documents into the new, unsharded collection on the new shard...
    assert.commandWorked(newShardColl.insert(insertedDocs));

    // ... and confirm that all the pre-existing streams see all of these events.
    for (let csCursor of [singleCollCS, wholeDBCS, wholeClusterCS]) {
        assertAllEventsObserved(csCursor, insertedDocs);
    }

    // Tear down this call's resources: close the cursors, drop both databases (this also clears
    // 'newShard1' as their primary shard, a precondition for removing it), formally remove the
    // shard from the cluster, and only then stop its process. Full cleanup here is required since
    // the next resume-mode call reuses these same names.
    wholeClusterCS.close();
    wholeDBCS.close();
    singleCollCS.close();
    assert.commandWorked(oldShardDB.runCommand({dropDatabase: 1}));
    assert.commandWorked(newShardDB.runCommand({dropDatabase: 1}));

    // removeShard()'s drain requires the balancer; only enable it for this narrow window.
    st.startBalancer();
    removeShard(st, "newShard1");
    st.stopBalancer();
    newShard1.stopSet();
}

setup();

testChangeStreamWillOpenCursorsOnNewShardCorrectly({resumeAfter: getResumeTokenNow()});
testChangeStreamWillOpenCursorsOnNewShardCorrectly({startAfter: getResumeTokenNow()});
testChangeStreamWillOpenCursorsOnNewShardCorrectly({startAtOperationTime: getOperationTimeNow()});

tearDown();
