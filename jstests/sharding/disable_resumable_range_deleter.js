/**
 * Verifies the effect of setting disableResumableRangeDeleter to true on a shard.
 *
 * requires_persistence - This test restarts shards and expects them to remember their data.
 * requires_fcv_47 - This test changed the behavior of disableResumableRangeDeleter from 4.4.
 * @tags: [requires_persistence, requires_fcv_47]
 */
(function() {

"use strict";

load("jstests/libs/fail_point_util.js");

// This test intentionally disables the resumable range deleter.
TestData.skipCheckOrphans = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

jsTest.log("Setup");
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

jsTest.log("Suspend range deletion and cause a range deletion task to be created on shard0.");
let suspendRangeDeletionFailpoint = configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

jsTest.log("Restart shard0 with disableResumableRangeDeleter=true.");
// Note, the suspendRangeDeletion failpoint will not survive the restart.
st.rs0.restart(0, {
    remember: true,
    appendOptions: true,
    startClean: false,
    setParameter: "disableResumableRangeDeleter=true"
});

jsTest.log("Shard0 should fail to submit the range deletion task on stepup.");
checkLog.contains(st.rs0.getPrimary(), "Failed to submit range deletion task");

jsTest.log("Shard0 should fail to receive a range that overlaps the range deletion task.");
// The error from moveChunk gets wrapped as an OperationFailed error, so we have to check the error
// message to find the original cause.
const moveChunkRes = st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName});
assert.commandFailedWithCode(moveChunkRes, ErrorCodes.OperationFailed);
assert(moveChunkRes.errmsg.indexOf("ResumableRangeDeleterDisabled") > -1);

jsTest.log("Shard0 should fail to do cleanupOrphaned on the namespace.");
assert.commandFailedWithCode(st.rs0.getPrimary().adminCommand({cleanupOrphaned: ns}),
                             ErrorCodes.ResumableRangeDeleterDisabled);

jsTest.log("Shard0 should be able to do cleanupOrphaned on an unrelated namespace.");
assert.commandWorked(st.rs0.getPrimary().adminCommand({cleanupOrphaned: "test.unrelated"}));

jsTest.log("Restart shard1 with disableResumableRangeDeleter=true.");
st.rs1.restart(0, {
    remember: true,
    appendOptions: true,
    startClean: false,
    setParameter: "disableResumableRangeDeleter=true"
});

jsTest.log("Shard0 should be able to donate a chunk and shard1 should be able to receive it.");
// disableResumableRangeDeleter should not prevent a shard from donating a chunk, and should not
// prevent a shard from receiving a chunk for which it doesn't have overlapping range deletion
// tasks.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: -1}, to: st.shard1.shardName}));

jsTest.log("Restart shard0 with disableResumableRangeDeleter=false.");
st.rs0.restart(0, {
    remember: true,
    appendOptions: true,
    startClean: false,
    setParameter: "disableResumableRangeDeleter=false"
});

jsTest.log("Shard0 should now be able to re-receive the chunk it failed to receive earlier.");
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));

st.stop();
})();
