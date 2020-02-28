/*
 * Tests that cleanupOrphaned cannot be run while the feature compatibility version is upgrading or
 * downgrading.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

jsTest.log("Move the chunk to shard1 and wait for the range deletion task on shard0 to finish.");
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}));

jsTest.log("Insert orphans directly onto shard0.");
for (let i = 0; i < 100; i++) {
    st.shard0.getDB(dbName).getCollection(collName).insert({x: i});
}
assert.eq(100, st.shard0.getDB(dbName).getCollection(collName).count());

//
// Test cleanupOrphaned when FCV is upgrading
//

jsTest.log("Ensure FCV 4.2 on shard0.");
assert.commandWorked(st.shard0.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(st.shard0.getDB("admin"), lastStableFCV);

jsTest.log("Send setFCV to shard0 and make it fail while the FCV is 'upgrading'.");
let failSetFCVUpgradeFailpoint = configureFailPoint(st.rs0.getPrimary(), 'failUpgrading');
assert.commandFailed(st.shard0.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.shard0.getDB("admin"), lastStableFCV, latestFCV);

jsTest.log("cleanupOrphaned should fail because the FCV is 'upgrading'.");
assert.commandFailedWithCode(st.shard0.adminCommand({cleanupOrphaned: ns}),
                             ErrorCodes.ConflictingOperationInProgress);
assert.eq(100, st.shard0.getDB(dbName).getCollection(collName).count());

//
// Test cleanupOrphaned when FCV is upgraded to 4.4
//

jsTest.log("Send setFCV to shard0 to finish the FCV upgrade.");
failSetFCVUpgradeFailpoint.off();
assert.commandWorked(st.shard0.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.shard0.getDB("admin"), latestFCV);

// The setFCV upgrade will have inserted a range deletion task for the unowned range.

jsTest.log("cleanupOrphaned should succeed after waiting for the orphans to be deleted.");
assert.commandWorked(st.shard0.adminCommand({cleanupOrphaned: ns}));
assert.eq(0, st.shard0.getDB(dbName).getCollection(collName).count());

//
// Test cleanupOrphaned when FCV is downgrading
//

jsTest.log("Insert orphans directly onto shard0 again.");
for (let i = 0; i < 100; i++) {
    st.shard0.getDB(dbName).getCollection(collName).insert({x: i});
}
assert.eq(100, st.shard0.getDB(dbName).getCollection(collName).count());

jsTest.log("Send setFCV to shard0 and make it fail while the FCV is 'downgrading'.");
let failSetFCVDowngradeFailpoint = configureFailPoint(st.rs0.getPrimary(), 'failDowngrading');
assert.commandFailed(st.shard0.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
// Note that when the FCV is 'downgrading', both the version and targetVersion are last stable.
checkFCV(st.shard0.getDB("admin"), lastStableFCV, lastStableFCV);

jsTest.log("cleanupOrphaned should fail because the FCV is 'downgrading'.");
assert.commandFailedWithCode(st.shard0.adminCommand({cleanupOrphaned: ns}),
                             ErrorCodes.ConflictingOperationInProgress);
assert.eq(100, st.shard0.getDB(dbName).getCollection(collName).count());

//
// Test cleanupOrphaned when FCV is fully downgraded to 4.2
//

jsTest.log("Send setFCV to shard0 to finish the FCV downgrade.");
failSetFCVDowngradeFailpoint.off();
assert.commandWorked(st.shard0.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(st.shard0.getDB("admin"), lastStableFCV);

jsTest.log("cleanupOrphaned should actively clean up the orphans.");
assert.eq(100, st.shard0.getDB(dbName).getCollection(collName).count());
assert.commandWorked(st.shard0.adminCommand({cleanupOrphaned: ns}));
assert.eq(0, st.shard0.getDB(dbName).getCollection(collName).count());

st.stop();
})();
