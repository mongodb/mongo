// Tests that a primary with upgrade featureCompatibilityVersion cannot connect with a secondary
// with a lower binary version.
(function() {
"use strict";

load("jstests/libs/feature_compatibility_version.js");
load("jstests/libs/write_concern_util.js");

const latest = "latest";
const downgrade = "last-stable";

// Start a new replica set with two latest version nodes.
let rst = new ReplSetTest({
    nodes: [{binVersion: latest}, {binVersion: latest, rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false}
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let latestSecondary = rst.getSecondary();

// Set the featureCompatibilityVersion to the downgrade version so that a downgrade node can
// join the set.
assert.commandWorked(
    primary.getDB("admin").runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Add a downgrade node to the set.
let downgradeSecondary = rst.add({binVersion: downgrade, rsConfig: {priority: 0}});
rst.reInitiate();

// Wait for the downgrade secondary to finish initial sync.
rst.awaitSecondaryNodes();
rst.awaitReplication();

// Stop replication on the downgrade secondary.
stopServerReplication(downgradeSecondary);

// Set the featureCompatibilityVersion to the upgrade version. This will not replicate to
// the downgrade secondary, but the downgrade secondary will no longer be able to
// communicate with the rest of the set.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Shut down the latest version secondary.
rst.stop(latestSecondary);

// The primary should step down, since it can no longer see a majority of the replica set.
rst.waitForState(primary, ReplSetTest.State.SECONDARY);

restartServerReplication(downgradeSecondary);
rst.stopSet();
})();
