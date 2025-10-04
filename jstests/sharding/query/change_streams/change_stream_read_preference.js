// Tests that change streams and their update lookups obey the read preference specified by the
// user.
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
import {enableLocalReadLogs, getLocalReadCount} from "jstests/libs/local_reads.js";
import {
    profilerHasAtLeastOneMatchingEntryOrThrow,
    profilerHasSingleMatchingEntryOrThrow,
} from "jstests/libs/profiler.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    name: "change_stream_read_pref",
    shards: 2,
    rs: {
        nodes: 2,
        // Use a higher frequency for periodic noops to speed up the test.
        setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
    },
});

const dbName = jsTestName();
const mongosDB = st.s0.getDB(dbName);
const mongosColl = mongosDB[jsTestName()];

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.rs0.getURL()}));

// Shard the test collection on _id.
assert.commandWorked(mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

// Move the [0, MaxKey] chunk to st.shard1.shardName.
assert.commandWorked(mongosDB.adminCommand({moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

// Turn on the profiler.
for (let rs of [st.rs0, st.rs1]) {
    assert.commandWorked(rs.getPrimary().getDB(dbName).setProfilingLevel(2));
    assert.commandWorked(rs.getSecondary().getDB(dbName).setProfilingLevel(2));
    enableLocalReadLogs(rs.getPrimary());
    enableLocalReadLogs(rs.getSecondary());
}

// Write a document to each chunk.
assert.commandWorked(mongosColl.insert({_id: -1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

// Test that change streams go to the primary by default.
let primaryStreamTest = new ChangeStreamTest(mongosDB);
let changeStreamComment = "change stream against primary";
const primaryStream = primaryStreamTest.startWatchingChanges({
    collection: mongosColl,
    pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
    aggregateOptions: {comment: changeStreamComment},
});

assert.commandWorked(mongosColl.update({_id: -1}, {$set: {updated: true}}));
assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updated: true}}));

let primaryUpdates = primaryStreamTest.getNextChanges(primaryStream, 2);
assert.eq(primaryUpdates[0].fullDocument, {_id: -1, updated: true});
assert.eq(primaryUpdates[1].fullDocument, {_id: 1, updated: true});

for (let rs of [st.rs0, st.rs1]) {
    const primaryDB = rs.getPrimary().getDB(dbName);
    // Test that the change stream itself goes to the primary. There might be more than one if
    // we needed multiple getMores to retrieve the changes.
    // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
    // because the initial aggregate will not show up.
    profilerHasAtLeastOneMatchingEntryOrThrow({
        profileDB: primaryDB,
        filter: {"originatingCommand.comment": changeStreamComment},
    });

    // Test that the update lookup goes to the primary as well.
    const localReadCount = getLocalReadCount(primaryDB, mongosColl.getFullName(), changeStreamComment);
    if (localReadCount === 0) {
        let filter = {
            op: "command",
            ns: mongosColl.getFullName(),
            "command.comment": changeStreamComment,
            "command.aggregate": mongosColl.getName(),
            "errName": {$ne: "StaleConfig"},
        };

        profilerHasSingleMatchingEntryOrThrow({profileDB: primaryDB, filter: filter});
    }
}

primaryStreamTest.cleanUp();

// Test that change streams go to the secondary when the readPreference is {mode: "secondary"}.
let secondaryStreamTest = new ChangeStreamTest(mongosDB);
changeStreamComment = "change stream against secondary";
let secondaryStream = secondaryStreamTest.startWatchingChanges({
    pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
    collection: mongosColl,
    aggregateOptions: {comment: changeStreamComment, $readPreference: {mode: "secondary"}},
});

assert.commandWorked(mongosColl.update({_id: -1}, {$set: {updatedCount: 2}}));
assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updatedCount: 2}}));

let secondaryUpdates = secondaryStreamTest.getNextChanges(secondaryStream, 2);
assert.eq(secondaryUpdates[0].fullDocument, {_id: -1, updated: true, updatedCount: 2});
assert.eq(secondaryUpdates[1].fullDocument, {_id: 1, updated: true, updatedCount: 2});

for (let rs of [st.rs0, st.rs1]) {
    const secondaryDB = rs.getSecondary().getDB(dbName);
    // Test that the change stream itself goes to the secondary. There might be more than one if
    // we needed multiple getMores to retrieve the changes.
    // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
    // because the initial aggregate will not show up.
    profilerHasAtLeastOneMatchingEntryOrThrow({
        profileDB: secondaryDB,
        filter: {"originatingCommand.comment": changeStreamComment},
    });

    // Test that the update lookup goes to the secondary as well.
    const localReadCount = getLocalReadCount(secondaryDB, mongosColl.getFullName(), changeStreamComment);
    if (localReadCount === 0) {
        let filter = {
            op: "command",
            ns: mongosColl.getFullName(),
            "command.comment": changeStreamComment,
            // We need to filter out any profiler entries with a stale config - this is the
            // first read on this secondary with a readConcern specified, so it is the first
            // read on this secondary that will enforce shard version.
            errCode: {$ne: ErrorCodes.StaleConfig},
            "command.aggregate": mongosColl.getName(),
        };
        profilerHasSingleMatchingEntryOrThrow({profileDB: secondaryDB, filter: filter});
    }
}

secondaryStreamTest.cleanUp();
st.stop();
