/*
 *  Tests that migrations work correctly across shards with mixed FCV state.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

function setup() {
    // Create 2 shards with 3 replicas each.
    let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

    // Create a sharded collection with two chunks: [-inf, 50), [50, inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

    let testDB = st.s.getDB(dbName);
    let testColl = testDB.foo;

    // Insert documents into both chunks on shard0.
    for (let i = 0; i < 100; i++) {
        testColl.insert({x: i});
    }

    return st;
}

function testMigrationSucceedsWhileUpgradingSource() {
    jsTestLog("Test that migration succeeds while upgrading source");

    let st = setup();

    // Ensure last-lts FCV on shard0.
    assert.commandWorked(st.shard0.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(st.shard0.getDB("admin"), lastLTSFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    // Fail while upgrading.
    let shard0Primary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(shard0Primary, 'failUpgrading');

    // Upgrade source.
    assert.commandFailed(st.shard0.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Move chunk [50, inf) to shard1 should succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    failpoint.off();

    st.stop();
}

function testMigrationSucceedsWhileDowngradingSource() {
    jsTestLog("Test that migration succeeds while downgrading source");

    let st = setup();

    // Ensure latest FCV on both shards.
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    // Fail while downgrading.
    let shard0Primary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(shard0Primary, 'failDowngrading');

    // Downgrade source.
    assert.commandFailed(st.shard0.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    // Move chunk [50, inf) to shard1 should succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    failpoint.off();

    st.stop();
}

function testMigrationSucceedsWhileUpgradingDestination() {
    jsTestLog("Test that migration succeeds while upgrading destination");

    let st = setup();

    // Ensure last-lts FCV on shard1.
    assert.commandWorked(
        st.shard1.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), lastLTSFCV);

    // Fail while upgrading.
    let shard1Primary = st.rs1.getPrimary();
    let failpoint = configureFailPoint(shard1Primary, 'failUpgrading');

    // Upgrade destination.
    assert.commandFailed(st.shard1.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Move chunk [50, inf) to shard1 should succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    failpoint.off();

    st.stop();
}

function testMigrationSucceedsWhileDowngradingDestination() {
    jsTestLog("Test that migration succeeds while downgrading destination");

    let st = setup();

    // Ensure latest FCV on both shards.
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    // Fail while downgrading.
    let shard1Primary = st.rs1.getPrimary();
    let failpoint = configureFailPoint(shard1Primary, 'failDowngrading');

    // Downgrade destination.
    assert.commandFailed(st.shard1.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    // Move chunk [50, inf) to shard1 should succeed.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    failpoint.off();

    st.stop();
}

function testMigrateFromLastLTSToLastLTS() {
    jsTestLog("Test last-lts FCV -> last-lts FCV");
    let st = setup();

    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(st.shard0.getDB("admin"), lastLTSFCV);
    checkFCV(st.shard1.getDB("admin"), lastLTSFCV);

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    st.stop();
}

function testMigrateFromLatestToLastLTS() {
    jsTestLog("Test latest FCV -> last-lts FCV");
    let st = setup();

    assert.commandWorked(
        st.shard1.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), lastLTSFCV);

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    st.stop();
}

function testMigrateFromLastLTSToLatest() {
    jsTestLog("Test last-lts FCV -> latest FCV fail");
    let st = setup();

    assert.commandWorked(
        st.shard0.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(st.shard0.getDB("admin"), lastLTSFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    assert.commandWorked(st.s.adminCommand({
        moveChunk: ns,
        find: {x: 50},
        to: st.shard1.shardName,
        secondaryThrottle: true,
        writeConcern: {w: 1}
    }));

    st.stop();
}

let moveChunk = function(ns, shard) {
    jsTestLog("Starting moveChunk " + ns + " " + shard);

    let adminDb = db.getSiblingDB("admin");
    assert.commandWorked(adminDb.runCommand({moveChunk: ns, find: {x: 50}, to: shard}));
};

function testSetFCVDoesNotBlockWhileMigratingChunk() {
    jsTestLog("Testing that setFCV does not block while migrating a chunk");
    let st = setup();

    // Set config and shards to last-lts FCV
    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    checkFCV(st.shard0.getDB("admin"), lastLTSFCV);
    checkFCV(st.shard1.getDB("admin"), lastLTSFCV);

    // Start migration and block with failpoint.
    let shard0Primary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(shard0Primary, "moveChunkHangAtStep2");

    // Move chunk [50, inf) to shard1.
    const awaitShell =
        startParallelShell(funWithArgs(moveChunk, ns, st.shard1.shardName), st.s.port);

    failpoint.wait();

    // Send FCV command with a maxTimeMS and assert that it does not timeout.
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {setFeatureCompatibilityVersion: latestFCV, maxTimeMS: 10000}));

    failpoint.off();
    awaitShell();

    st.stop();
}

testMigrationSucceedsWhileUpgradingSource();
testMigrationSucceedsWhileDowngradingSource();
testMigrationSucceedsWhileUpgradingDestination();
testMigrationSucceedsWhileDowngradingDestination();
testMigrateFromLastLTSToLastLTS();
testMigrateFromLatestToLastLTS();
testMigrateFromLastLTSToLatest();
testSetFCVDoesNotBlockWhileMigratingChunk();
})();
