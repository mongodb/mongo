/**
 * Tests that the 'appendOplogNote' command on mongos correctly performs a no-op write on each
 * shard and advances the $clusterTime.
 *
 * @tags: [requires_fcv_60]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function checkOplogEntry(actualOplogEntry) {
    const {op, o} = actualOplogEntry;
    assert.eq({op, o}, {op: "n", o: {a: 2}}, actualOplogEntry);
}

// We need to disable the periodic no-op writer so we can verify that the 'appendOplogNote'
// no-op is the latest operation on the oplog.
const st = new ShardingTest(
    {mongos: 1, shards: 2, rs: {nodes: 2, setParameter: {writePeriodicNoops: false}}});
const admin = st.getDB('admin');
const shardOnePrimary = st.rs0.getPrimary();
const shardTwoPrimary = st.rs1.getPrimary();

// Test that issuing the command without the 'data' field results in an error.
let res = assert.commandFailed(admin.runCommand({appendOplogNote: 1}));
assert.eq(res.code, ErrorCodes.NoSuchKey);
assert(res.hasOwnProperty("raw"), res);

// Test that the error response contains the correct fields.
const appendOplogNoteFailpoint = configureFailPoint(shardOnePrimary, "failCommand", {
    errorCode: ErrorCodes.HostUnreachable,
    failCommands: ["appendOplogNote"],
    failInternalCommands: true
});

res = assert.commandFailed(admin.runCommand({appendOplogNote: 1, data: {a: 1}}));
assert(res.hasOwnProperty("errmsg"), res);
assert.eq(res.code, ErrorCodes.HostUnreachable);
assert(res.hasOwnProperty("raw"), res);

appendOplogNoteFailpoint.wait();
appendOplogNoteFailpoint.off();

// Force a database refresh on the sharding side so that the corresponding config.cache.databases
// entry is already in the oplog before we start issuing successful appendOplogNote commands.
assert.commandWorked(shardOnePrimary.adminCommand({_flushDatabaseCacheUpdates: "config"}));
assert.commandWorked(shardTwoPrimary.adminCommand({_flushDatabaseCacheUpdates: "config"}));

// Test that a successful 'appendOplogNote' command performs a no-op write and advances the
// $clusterTime.
const shardOneBefore =
    assert.commandWorked(shardOnePrimary.getDB("admin").runCommand({replSetGetStatus: 1}));
const shardTwoBefore =
    assert.commandWorked(shardTwoPrimary.getDB("admin").runCommand({replSetGetStatus: 1}));

res = assert.commandWorked(admin.runCommand({appendOplogNote: 1, data: {a: 2}}));
assert(res.hasOwnProperty("raw"), res);

const shardOneAfter =
    assert.commandWorked(shardOnePrimary.getDB("admin").runCommand({replSetGetStatus: 1}));
const shardTwoAfter =
    assert.commandWorked(shardTwoPrimary.getDB("admin").runCommand({replSetGetStatus: 1}));

assert.lt(shardOneBefore.members[0].optime.ts, shardOneAfter.members[0].optime.ts);
assert.lt(shardTwoBefore.members[0].optime.ts, shardTwoAfter.members[0].optime.ts);

let lastEntryShardOne =
    shardOnePrimary.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).toArray()[0];
let lastEntryShardTwo =
    shardTwoPrimary.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).toArray()[0];

// The $clusterTime in the 'replSetGetStatus' response should be equal to the timestamp of
// 'appendOplogNote' command no-op write, which is the last operation on the shard oplogs.
checkOplogEntry(lastEntryShardOne);
checkOplogEntry(lastEntryShardTwo);

assert.eq(shardOneAfter.members[0].optime.ts, lastEntryShardOne.ts);
assert.eq(shardTwoAfter.members[0].optime.ts, lastEntryShardTwo.ts);

st.stop();
}());
