/*
 * Ensures that error is reported when overlapping range is submitted for deletion.
 * @tags: [assumes_balancer_off]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");
load("jstests/libs/fail_point_util.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Create 2 shards with 3 replicas each.
let st =
    new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}, other: {enableBalancer: false}});

// Increase timeout for range deletion of overlapping range on recipient.
st.shard0.rs.getPrimary().adminCommand(
    {setParameter: 1, receiveChunkWaitForRangeDeleterTimeoutMS: 90000});
st.shard1.rs.getPrimary().adminCommand(
    {setParameter: 1, receiveChunkWaitForRangeDeleterTimeoutMS: 90000});

(() => {
    jsTestLog("Test simple shard key");

    // Create a sharded collection with one chunk and a single-field shard key.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    // Pause range deletion on shard0.
    let suspendRangeDeletionFailpoint = configureFailPoint(st.shard0, "suspendRangeDeletion");

    // Move the only chunk from shard0 to shard1. This will leave orphans on shard0 since we paused
    // range deletion.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    // Move the only chunk back to shard0 and expect timeout failure, since range deletion was
    // paused and there are orphans on shard0.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard0.shardName, maxTimeMS: 5000}),
        ErrorCodes.MaxTimeMSExpired);

    suspendRangeDeletionFailpoint.off();

    // The moveChunk should now be unblocked and succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard0.shardName}));

    st.s.getCollection(ns).drop();
})();

(() => {
    jsTestLog("Test hashed shard key");

    // Create a sharded collection with one chunk and a hashed shard key.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 'hashed'}}));

    // Pause range deletion on shard0.
    let suspendRangeDeletionFailpoint = configureFailPoint(st.shard0, "suspendRangeDeletion");

    // Move the only chunk from shard0 to shard1. This will leave orphans on shard0 since we paused
    // range deletion.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    // Move the only chunk back to shard0 and expect timeout failure, since range deletion was
    // paused and there are orphans on shard0.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard0.shardName, maxTimeMS: 5000}),
        ErrorCodes.MaxTimeMSExpired);

    suspendRangeDeletionFailpoint.off();

    // The moveChunk should now be unblocked and succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard0.shardName}));

    st.s.getCollection(ns).drop();
})();

(() => {
    jsTestLog("Test compound shard key");

    // Create a sharded collection with one chunk and a compound shard key.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1, y: 1}}));

    // Pause range deletion on shard0.
    let suspendRangeDeletionFailpoint = configureFailPoint(st.shard0, "suspendRangeDeletion");

    // Move the only chunk from shard0 to shard1. This will leave orphans on shard0 since we paused
    // range deletion.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50, y: 50}, to: st.shard1.shardName}));

    // Move the only chunk back to shard0 and expect timeout failure, since range deletion was
    // paused and there are orphans on shard0.
    assert.commandFailedWithCode(
        st.s.adminCommand(
            {moveChunk: ns, find: {x: 50, y: 50}, to: st.shard0.shardName, maxTimeMS: 5000}),
        ErrorCodes.MaxTimeMSExpired);

    suspendRangeDeletionFailpoint.off();

    // The moveChunk should now be unblocked and succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50, y: 50}, to: st.shard0.shardName}));

    st.s.getCollection(ns).drop();
})();

st.stop();
})();
