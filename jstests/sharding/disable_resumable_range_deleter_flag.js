/*
 *  Tests that migrations behave correctly when the resumable range deleter protocol is
 * disabled.
 *
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const dbName = "test";

let st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

let moveChunk = function(ns, shard) {
    jsTestLog("Starting moveChunk " + ns + " " + shard);

    let adminDb = db.getSiblingDB("admin");
    assert.commandWorked(adminDb.runCommand({moveChunk: ns, find: {x: 50}, to: shard}));
};

function testBothDisabledSucceeds() {
    jsTestLog("Test that disabled donor and recipient succeeds migration");

    const [collName, ns] = getNewNs(dbName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    // Insert documents into both chunks on shard0.
    let testColl = st.s.getDB(dbName).getCollection(collName);
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    // Disable resumable range deleter on both shards.
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": true}));
    assert.commandWorked(st.shard1.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": true}));

    // Move chunk [50, inf) to shard1 should succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    // Re-enable resumable range delete.
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": false}));
    assert.commandWorked(st.shard1.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": false}));
}

function testDisabledSourceFailsMigration() {
    jsTestLog("Test that disabled donor fails migration");

    const [collName, ns] = getNewNs(dbName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    // Insert documents into both chunks on shard0.
    let testColl = st.s.getDB(dbName).getCollection(collName);
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    // Disable resumable range deleter on shard0.
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": true}));

    // Move chunk [50, inf) to shard1 should fail since migration id is missing.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);

    // Re-enable resumable range deleter on shard0.
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": false}));
}

function testDisabledRecipientSucceedsMigration() {
    jsTestLog("Test that disabled recipient succeeds migration");

    const [collName, ns] = getNewNs(dbName);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    // Insert documents into both chunks on shard0.
    let testColl = st.s.getDB(dbName).getCollection(collName);
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    // Disable resumable range deleter on shard1.
    assert.commandWorked(st.shard1.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": true}));

    // Move chunk [50, inf) to shard1 should succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    // Re-enable resumable range deleter on shard1.
    assert.commandWorked(st.shard1.getDB("admin").runCommand(
        {setParameter: 1, "disableResumableRangeDeleter": false}));
}

testBothDisabledSucceeds();
testDisabledSourceFailsMigration();
testDisabledRecipientSucceedsMigration();

st.stop();
})();
