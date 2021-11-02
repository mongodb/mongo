/**
 * Tests that a moveChunk operation is properly aborted when an index command is received from a
 * non-internal client while in the critical section.
 *
 * @tags: [
 *   does_not_support_stepdowns
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const dbName = "test";
const collName = "move_chunk_critical_section_non_internal_client_abort";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 0}));
assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 1}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Pause the moveChunk while it is in the critical section.
const fp = configureFailPoint(st.shard0, "moveChunkHangAtStep5");
const awaitResult =
    startParallelShell(funWithArgs(function(ns, toShardName) {
                           assert.commandFailedWithCode(
                               db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                               ErrorCodes.Interrupted);
                       }, ns, st.shard1.shardName), st.s.port);
fp.wait();

// Perform a collMod directly on the shard's primary to cause the moveChunk to abort.
assert.commandWorked(
    st.rs0.getPrimary().getDB(dbName).runCommand({collMod: collName, writeConcern: {w: 1}}));

fp.off();
awaitResult();

st.stop();
})();
