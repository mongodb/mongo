/*
 * Tests a possible lock-free read scenario wherein the collection on a namespace goes from
 * unsharded (sent in the request to the shard) to sharded (which the lock-free state finds) to
 * dropped and recreated (which the subsequent shardVersion check finds) while the lock-free state
 * is being set up for a read. The lock-free state must ensure consistency with the sharding state.
 * In such a case, the lock-free read state setup should be retried internally until consistent.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");         // configureFailPoint
load("jstests/libs/parallel_shell_helpers.js");  // startParallelShell

const st = new ShardingTest({mongos: 1, config: 1, shards: 1, rs: {nodes: 1}});

// This logical session ID is used to identify the particular find command for the failpoints.
const lsid = ({id: UUID()});

const hangBeforeAutoGetCollectionLockFreeShardedStateAccessFp =
    configureFailPoint(st.shard0.rs.nodes[0],
                       "hangBeforeAutoGetCollectionLockFreeShardedStateAccess",
                       {lsid: lsid.id});

const reachedAutoGetLockFreeShardConsistencyRetryFp = configureFailPoint(
    st.shard0.rs.nodes[0], "reachedAutoGetLockFreeShardConsistencyRetry", {lsid: lsid.id});

const hangBeforeAutoGetShardVersionCheckFp = configureFailPoint(
    st.shard0.rs.nodes[0], "hangBeforeAutoGetShardVersionCheck", {lsid: lsid.id});

const dbName = "test";
const collName = "lock_free_read_unsharded_sharded_unsharded";
const collection = st.s.getCollection(`${dbName}.${collName}`);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(collection.runCommand("create"));

// Establish state on the mongos for the new collection as unsharded.
collection.find().toArray();

// Start a new read routed through the mongos that will send an UNSHARDED version with the request
// to the mongod.
const awaitFind = startParallelShell(
    funWithArgs(function(lsid, ns) {
        const mongos = db.getMongo();
        assert.commandWorked(mongos.getCollection(ns).runCommand("find", {lsid}));
    }, lsid, collection.getFullName()), st.s.port);

// Pause the operation right before lock-free read setup checks whether or not the collection is
// sharded.
hangBeforeAutoGetCollectionLockFreeShardedStateAccessFp.wait();

// Shard the collection so the lock-free read setup will find the collection sharded.
assert.commandWorked(st.s.adminCommand({shardCollection: collection.getFullName(), key: {_id: 1}}));

// Unpause the lock-free state to find a sharded collection, then wait for it to pause before the SV
// check.
hangBeforeAutoGetCollectionLockFreeShardedStateAccessFp.off();
hangBeforeAutoGetShardVersionCheckFp.wait();

// Drop the sharded collection and recreate a collection on the same namespace that's unsharded.
assert.commandWorked(collection.runCommand("drop"));
assert.commandWorked(collection.runCommand("create"));

// Release the lock-free read.
hangBeforeAutoGetShardVersionCheckFp.off();

// Make sure we reach the point in the code where the inconsistency is detected, before proceeding
reachedAutoGetLockFreeShardConsistencyRetryFp.wait();
reachedAutoGetLockFreeShardConsistencyRetryFp.off();

awaitFind();

st.stop();
})();
