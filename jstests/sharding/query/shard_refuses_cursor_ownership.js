/**
 * This test runs and unsharded $out query which results in mongos setting up a cursor and giving
 * it to a shard to complete. mongos assumes the shard will kill the cursor, but if a shard doesn't
 * accept ownership of the cursor then previously no one would kill it. this test ensures mongos
 * will kill the cursor if a shard doesn't accept ownership.
 * @tags: [
 *     requires_fcv_62
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const st = new ShardingTest({shards: 2});

const dbName = jsTestName();
const collName = "foo";
const ns = dbName + "." + collName;

let db = st.s.getDB(dbName);
assert.commandWorked(db.dropDatabase());
let coll = db[collName];

st.shardColl(collName, {x: 1}, {x: 0}, {x: 1}, dbName, true);

assert.commandWorked(coll.insert([{x: -2}, {x: -1}, {x: 1}, {x: 2}]));

const primary = st.getPrimaryShard(dbName);
const other = st.getOther(st.getPrimaryShard(dbName));

// Start an aggregation that requires merging on a shard. Let it run until the shard cursors have
// been established but make it hang right before opening the merge cursor.
let shardedAggregateHangBeforeDispatchMergingPipelineFP =
    configureFailPoint(st.s, "shardedAggregateHangBeforeDispatchMergingPipeline");
let awaitAggregationShell = startParallelShell(
    funWithArgs((dbName, collName) => {
        assert.eq(
            0, db.getSiblingDB(dbName)[collName].aggregate([{$out: collName + ".out"}]).itcount());
    }, dbName, collName), st.s.port);
shardedAggregateHangBeforeDispatchMergingPipelineFP.wait();

// Start a chunk migration, let it run until it enters the critical section.
let hangBeforePostMigrationCommitRefresh =
    configureFailPoint(primary, "hangBeforePostMigrationCommitRefresh");
let awaitMoveChunkShell = startParallelShell(
    funWithArgs((recipientShard, ns) => {
        assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: -1}, to: recipientShard}));
    }, other.shardName, ns), st.s.port);
hangBeforePostMigrationCommitRefresh.wait();

// Let the aggregation continue and try to establish the merge cursor (it will first fail because
// the shard is in the critical section. Mongos will transparently retry).
shardedAggregateHangBeforeDispatchMergingPipelineFP.off();

// Let the migration exit the critical section and complete.
hangBeforePostMigrationCommitRefresh.off();

// The aggregation will be able to complete now.
awaitAggregationShell();

awaitMoveChunkShell();

// Did any cursor leak?
const idleCursors = primary.getDB("admin")
                        .aggregate([
                            {$currentOp: {idleCursors: true, allUsers: true}},
                            {$match: {type: "idleCursor", ns: ns}}
                        ])
                        .toArray();
assert.eq(0, idleCursors.length, "Found idle cursors: " + tojson(idleCursors));

// Check that range deletions can be completed (if a cursor was left open, the range deletion would
// not finish).
assert.soon(
    () => {
        return primary.getDB("config")["rangeDeletions"].find().itcount() === 0;
    },
    "Range deletion tasks did not finish: + " +
        tojson(primary.getDB("config")["rangeDeletions"].find().toArray()));

st.stop();
})();
