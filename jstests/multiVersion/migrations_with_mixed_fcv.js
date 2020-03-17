/*
 *  Tests that migrations work correctly across shards with mixed FCV state.
 *  @tags: [fix_for_fcv_46]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

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

function testMigrationFailsWhileUpgradingSource() {
    jsTestLog("Test fail while upgrading source");

    let st = setup();

    // Ensure FCV 4.2 on shard0.
    assert.commandWorked(st.shard0.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.shard0.getDB("admin"), lastStableFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    // Fail while upgrading.
    let shard0Primary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(shard0Primary, 'failUpgrading');

    // Upgrade source.
    assert.commandFailed(st.shard0.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Move chunk [50, inf) to shard1 should fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);

    failpoint.off();

    st.stop();
}

function testMigrationFailsWhileDowngradingSource() {
    jsTestLog("Test fail while downgrading source");

    let st = setup();

    // Ensure FCV 4.4 on both shards.
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    // Fail while downgrading.
    let shard0Primary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(shard0Primary, 'failDowngrading');

    // Downgrade source.
    assert.commandFailed(st.shard0.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    // Move chunk [50, inf) to shard1 should fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);

    failpoint.off();

    st.stop();
}

function testMigrationFailsWhileUpgradingDestination() {
    jsTestLog("Test fail while upgrading destination");

    let st = setup();

    // Ensure FCV 4.2 on shard1.
    assert.commandWorked(
        st.shard1.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), lastStableFCV);

    // Fail while upgrading.
    let shard1Primary = st.rs1.getPrimary();
    let failpoint = configureFailPoint(shard1Primary, 'failUpgrading');

    // Upgrade destination.
    assert.commandFailed(st.shard1.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Move chunk [50, inf) to shard1 should fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);

    failpoint.off();

    st.stop();
}

function testMigrationFailsWhileDowngradingDestination() {
    jsTestLog("Test fail while downgrading destination");

    let st = setup();

    // Ensure FCV 4.4 on both shards.
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    // Fail while downgrading.
    let shard1Primary = st.rs1.getPrimary();
    let failpoint = configureFailPoint(shard1Primary, 'failDowngrading');

    // Downgrade destination.
    assert.commandFailed(st.shard1.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    // Move chunk [50, inf) to shard1 should fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);

    failpoint.off();

    st.stop();
}

function testMigrateFromLastStableToLastStable() {
    jsTestLog("Test FCV 4.2 -> FCV 4.2");
    let st = setup();

    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.shard0.getDB("admin"), lastStableFCV);
    checkFCV(st.shard1.getDB("admin"), lastStableFCV);

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    st.stop();
}

function testMigrateFromLatestToLastStable() {
    jsTestLog("Test FCV 4.4 -> FCV 4.2");
    let st = setup();

    assert.commandWorked(
        st.shard1.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.shard0.getDB("admin"), latestFCV);
    checkFCV(st.shard1.getDB("admin"), lastStableFCV);

    // Move chunk [50, inf) to shard1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 50}, to: st.shard1.shardName}));

    st.stop();
}

function testMigrateFromLastStableToLatest() {
    jsTestLog("Test FCV 4.2 -> FCV 4.4 fail");
    let st = setup();

    assert.commandWorked(
        st.shard0.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(st.shard0.getDB("admin"), lastStableFCV);
    checkFCV(st.shard1.getDB("admin"), latestFCV);

    // Move chunk [50, inf) to shard1 should fail. Since shard1 is running FCV 4.4, it expects
    // _recvChunkStart to include explicit writeConcern. Since shard0 is running FCV 4.2, it will
    // not add it automatically. So we pass explicit writeConcern to the mongos moveChunk command
    // (which also requires secondaryThrottle: true), which causes it to be passed through
    // explicitly to shard0, which will use it when calling _recvChunkStart on shard1
    assert.commandFailedWithCode(st.s.adminCommand({
        moveChunk: ns,
        find: {x: 50},
        to: st.shard1.shardName,
        secondaryThrottle: true,
        writeConcern: {w: 1}
    }),
                                 ErrorCodes.ConflictingOperationInProgress);

    st.stop();
}

let moveChunk = function(ns, shard) {
    jsTestLog("Starting moveChunk " + ns + " " + shard);

    let adminDb = db.getSiblingDB("admin");
    assert.commandWorked(adminDb.runCommand({moveChunk: ns, find: {x: 50}, to: shard}));
};

function testSetFCVBlocksWhileMigratingChunk() {
    jsTestLog("Testing that setFCV blocks while migrating a chunk");
    let st = setup();

    // Set config and shards to FCV 4.2
    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    checkFCV(st.shard0.getDB("admin"), lastStableFCV);
    checkFCV(st.shard1.getDB("admin"), lastStableFCV);

    // Start migration and block with failpoint.
    let shard0Primary = st.rs0.getPrimary();
    let failpoint = configureFailPoint(shard0Primary, "moveChunkHangAtStep2");

    // Move chunk [50, inf) to shard1.
    const awaitShell =
        startParallelShell(funWithArgs(moveChunk, ns, st.shard1.shardName), st.s.port);

    failpoint.wait();

    // Send FCV command and wait for timeout.
    assert.commandFailedWithCode(st.shard0.getDB("admin").runCommand(
                                     {setFeatureCompatibilityVersion: latestFCV, maxTimeMS: 1000}),
                                 ErrorCodes.MaxTimeMSExpired);

    failpoint.off();
    awaitShell();

    st.stop();
}

testMigrationFailsWhileUpgradingSource();
testMigrationFailsWhileDowngradingSource();
testMigrationFailsWhileUpgradingDestination();
testMigrationFailsWhileDowngradingDestination();

testMigrateFromLastStableToLastStable();
testMigrateFromLatestToLastStable();
testMigrateFromLastStableToLatest();
testSetFCVBlocksWhileMigratingChunk();
})();
