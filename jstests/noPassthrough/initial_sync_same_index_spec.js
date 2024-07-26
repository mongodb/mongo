/**
 * Test that initial sync does not fail if an identical index spec is created, dropped and
 * recreated.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");  // for kDefaultWaitForFailPointTimeout

const testName = "initial_sync_same_index_spec";
const dbName = testName;
const collName = "testcoll";

// Set up a stable two node replica set.
let replTest = new ReplSetTest({
    name: testName,
    nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

let primaryDB = primary.getDB(dbName);
let secondaryDB = secondary.getDB(dbName);

let primaryColl = primaryDB[collName];
let secondaryColl = secondaryDB[collName];

// Insert data into the collection for initial sync to copy.
assert.commandWorked(primaryColl.insert([{_id: 1, a: 1}, {_id: 2, a: 2}]));

// Set WC to 1. The default WC is majority and the replica set will not be able to satisfy majority
// index create/drop later.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

/**
 * Restart the the secondary with no data in order to provoke an initial sync from the primary.
 * Use startup setParameter to set failpoints to pause the initial sync before indexes are fetched.
 */

const fp = 'hangBeforeClonerStage';
let secondaryStartupParams = {};
secondaryStartupParams['failpoint.' + fp] = tojson({
    mode: 'alwaysOn',
    data: {cloner: "CollectionCloner", stage: "listIndexes", nss: primaryColl.getFullName()}
});

// Only try initial sync once, so any failure syncing indexes will be surfaced.
secondaryStartupParams['numInitialSyncAttempts'] = 1;

const startupOptions = {
    startClean: true,
    setParameter: secondaryStartupParams
};

jsTestLog(
    "Restarting the the secondary with no data in order to provoke an initial sync from the " +
    "primary. Using startup options: " + tojson(startupOptions));
secondary = replTest.restart(secondary, startupOptions);
secondaryDB = secondary.getDB(dbName);
secondaryColl = secondaryDB[collName];

jsTestLog("Waiting for secondary to reach failPoint '" + fp + "'");
assert.commandWorked(secondary.adminCommand(
    {waitForFailPoint: fp, timesEntered: 1, maxTimeMS: kDefaultWaitForFailPointTimeout}));

// Restarting the secondary may have resulted in an election.  Wait until the system stabilizes and
// reaches RS_STARTUP2 state.
replTest.getPrimary();
replTest.waitForState(secondary, ReplSetTest.State.STARTUP_2);

/**
 * Now that the secondary is hanging in initial sync, create, drop and recreate identical indexes to
 * ensure it is correctly handled by initial sync (when the failpoint is removed).
 */

jsTestLog("Creating index on the primary.");
assert.commandWorked(primaryColl.createIndex({a: 1}, {name: "a1"}));

jsTestLog("Dropping index on the primary.");
assert.commandWorked(primaryColl.dropIndex("a1"));

jsTestLog("Recreating index with same spec on the primary.");
assert.commandWorked(primaryColl.createIndex({a: 1}, {name: "a2"}));

jsTestLog("Allowing secondary initial sync to resume.");
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "hangBeforeClonerStage", mode: 'off'}));

jsTestLog("Waiting for initial sync to complete successfully.");
replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);

replTest.stopSet();
})();
