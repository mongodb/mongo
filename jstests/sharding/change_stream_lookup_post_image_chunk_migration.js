// Tests that change stream with 'fullDocument: updateLookup' works correctly when chunks are
// migrated after updates are performed.
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
(function() {
'use strict';

load('jstests/libs/profiler.js');  // For various profiler helpers.

const st = new ShardingTest({
    shards: 3,
    rs: {nodes: 1, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}}
});

const dbName = jsTestName();
const collName = jsTestName();
const ns = dbName + "." + collName;

const mongos = st.s0;
const mongosColl = mongos.getDB(dbName).getCollection(collName);

const shard0 = st.shard0;
const shard1 = st.shard1;
const shard2 = st.shard2;

// Enable sharding to inform mongos of the database, and make sure all chunks start on shard 0.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, shard0.shardName);

// Create a change stream before any insert or update operations are performed.
const csBeforeSharding = mongosColl.watch([], {fullDocument: "updateLookup"});

// Insert four documents, and then read these 'insert' events from the change stream.
let next;
for (const id of [0, 1, 2, 3]) {
    assert.commandWorked(mongosColl.insert({_id: id}));
    assert.soon(() => csBeforeSharding.hasNext());
    next = csBeforeSharding.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.fullDocument, {_id: id});
}

// Save a resume token after the 'insert' operations and then close the change stream.
const resumeTokenBeforeUpdates = next._id;
csBeforeSharding.close();

function checkUpdateEvents(changeStream, csComment, idShardPairs) {
    for (const [id, shard] of idShardPairs) {
        assert.soon(() => changeStream.hasNext());
        const next = changeStream.next();
        assert.eq(next.operationType, "update");
        assert.eq(next.fullDocument, {_id: id, updated: true});

        // Test that each update lookup goes to the appropriate shard.
        const filter = {
            op: "command",
            ns: ns,
            "command.comment": csComment,
            errCode: {$ne: ErrorCodes.StaleConfig},
            "command.aggregate": collName,
            "command.pipeline.0.$match._id": id
        };

        profilerHasSingleMatchingEntryOrThrow({profileDB: shard.getDB(dbName), filter: filter});
    }
}

// Update two of the documents.
assert.commandWorked(mongosColl.update({_id: 0}, {$set: {updated: true}}));
assert.commandWorked(mongosColl.update({_id: 2}, {$set: {updated: true}}));

// Drop the 'profile' tables and then enable profiling on all shards.
for (const shard of [shard0, shard1, shard2]) {
    const db = shard.getDB(dbName);
    assert.commandWorked(db.setProfilingLevel(0));
    db.system.profile.drop();
    assert.commandWorked(db.setProfilingLevel(2));
}

// Now, actually shard the collection.
jsTestLog("Sharding collection");
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Split the collection into two chunks: [MinKey, 2) and [2, MaxKey].
assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 2}}));

jsTestLog("Migrating [MinKey, 2) to shard1 and [2, MaxKey] to shard2.");

for (const [id, shard] of [[0, shard1], [2, shard2]]) {
    const dest = shard.shardName;
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: id}, to: dest, _waitForDelete: true}));
}

// After sharding the collection and moving the documents to different shards, create a change
// stream with a resume token from before the collection was sharded.
const commentAfterSharding = "change stream after sharding";
const csAfterSharding = mongosColl.watch([], {
    resumeAfter: resumeTokenBeforeUpdates,
    fullDocument: "updateLookup",
    comment: commentAfterSharding
});

// Next two events in csAfterSharding should be the two 'update' operations.
checkUpdateEvents(csAfterSharding, commentAfterSharding, [[0, shard1], [2, shard2]]);

csAfterSharding.close();

// Update the other two documents.
assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updated: true}}));
assert.commandWorked(mongosColl.update({_id: 3}, {$set: {updated: true}}));

// Add a new shard to the cluster
const rs3 = new ReplSetTest({
    name: "shard3",
    nodes: 1,
    setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
});
rs3.startSet({shardsvr: ''});
rs3.initiate();
const shard3 = rs3.getPrimary();
shard3.shardName = "shard3";
assert.commandWorked(mongos.adminCommand({addShard: rs3.getURL(), name: shard3.shardName}));

// Drop the 'profile' tables and then enable profiling on all shards.
for (const shard of [shard0, shard1, shard2, shard3]) {
    const db = shard.getDB(dbName);
    assert.commandWorked(db.setProfilingLevel(0));
    db.system.profile.drop();
    assert.commandWorked(db.setProfilingLevel(2));
}

jsTestLog("Migrating [MinKey, 2) to shard2 and [2, MaxKey] to shard3.");

for (const [id, shard] of [[0, shard2], [2, shard3]]) {
    const dest = shard.shardName;
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: id}, to: dest, _waitForDelete: true}));
}

// After adding the new shard and migrating the documents to the new shard, create a change stream
// with a resume token from before the collection was sharded.
const commentAfterAddShard = "change stream after addShard";
const csAfterAddShard = mongosColl.watch([], {
    resumeAfter: resumeTokenBeforeUpdates,
    fullDocument: "updateLookup",
    comment: commentAfterAddShard
});

// Next four events in csAfterAddShard should be the four 'update' operations.
checkUpdateEvents(
    csAfterAddShard, commentAfterAddShard, [[0, shard2], [2, shard3], [1, shard2], [3, shard3]]);

csAfterAddShard.close();

st.stop();
rs3.stopSet();
})();
