// Tests that change streams and their update lookups obey the read preference specified by the
// user.
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
(function() {
"use strict";

load('jstests/libs/profiler.js');  // For various profiler helpers.

const st = new ShardingTest({
    name: "change_stream_read_pref",
    shards: 2,
    rs: {
        nodes: 2,
        // Use a higher frequency for periodic noops to speed up the test.
        setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
    },
});

const dbName = jsTestName();
const mongosDB = st.s0.getDB(dbName);
const mongosColl = mongosDB[jsTestName()];

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

// Shard the test collection on _id.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

// Move the [0, MaxKey] chunk to st.shard1.shardName.
assert.commandWorked(mongosDB.adminCommand(
    {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

// Turn on the profiler.
for (let rs of [st.rs0, st.rs1]) {
    assert.commandWorked(rs.getPrimary().getDB(dbName).setProfilingLevel(2));
    assert.commandWorked(rs.getSecondary().getDB(dbName).setProfilingLevel(2));
}

// Write a document to each chunk.
assert.commandWorked(mongosColl.insert({_id: -1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

// Test that change streams go to the primary by default.
let changeStreamComment = "change stream against primary";
const primaryStream = mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}],
                                           {comment: changeStreamComment});

assert.commandWorked(mongosColl.update({_id: -1}, {$set: {updated: true}}));
assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updated: true}}));

assert.soon(() => primaryStream.hasNext());
assert.eq(primaryStream.next().fullDocument, {_id: -1, updated: true});
assert.soon(() => primaryStream.hasNext());
assert.eq(primaryStream.next().fullDocument, {_id: 1, updated: true});

for (let rs of [st.rs0, st.rs1]) {
    const primaryDB = rs.getPrimary().getDB(dbName);
    // Test that the change stream itself goes to the primary. There might be more than one if
    // we needed multiple getMores to retrieve the changes.
    // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
    // because the initial aggregate will not show up.
    profilerHasAtLeastOneMatchingEntryOrThrow(
        {profileDB: primaryDB, filter: {'originatingCommand.comment': changeStreamComment}});

    // Test that the update lookup goes to the primary as well.
    let filter = {
        op: "command",
        ns: mongosColl.getFullName(),
        "command.comment": changeStreamComment,
        "command.aggregate": mongosColl.getName()
    };

    profilerHasSingleMatchingEntryOrThrow({profileDB: primaryDB, filter: filter});
}

primaryStream.close();

// Test that change streams go to the secondary when the readPreference is {mode: "secondary"}.
changeStreamComment = 'change stream against secondary';
const secondaryStream =
    mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}],
                         {comment: changeStreamComment, $readPreference: {mode: "secondary"}});

assert.commandWorked(mongosColl.update({_id: -1}, {$set: {updatedCount: 2}}));
assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updatedCount: 2}}));

assert.soon(() => secondaryStream.hasNext());
assert.eq(secondaryStream.next().fullDocument, {_id: -1, updated: true, updatedCount: 2});
assert.soon(() => secondaryStream.hasNext());
assert.eq(secondaryStream.next().fullDocument, {_id: 1, updated: true, updatedCount: 2});

for (let rs of [st.rs0, st.rs1]) {
    const secondaryDB = rs.getSecondary().getDB(dbName);
    // Test that the change stream itself goes to the secondary. There might be more than one if
    // we needed multiple getMores to retrieve the changes.
    // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
    // because the initial aggregate will not show up.
    profilerHasAtLeastOneMatchingEntryOrThrow(
        {profileDB: secondaryDB, filter: {'originatingCommand.comment': changeStreamComment}});

    // Test that the update lookup goes to the secondary as well.
    let filter = {
        op: "command",
        ns: mongosColl.getFullName(),
        "command.comment": changeStreamComment,
        // We need to filter out any profiler entries with a stale config - this is the
        // first read on this secondary with a readConcern specified, so it is the first
        // read on this secondary that will enforce shard version.
        errCode: {$ne: ErrorCodes.StaleConfig},
        "command.aggregate": mongosColl.getName()
    };

    profilerHasSingleMatchingEntryOrThrow({profileDB: secondaryDB, filter: filter});
}

secondaryStream.close();
st.stop();
}());
