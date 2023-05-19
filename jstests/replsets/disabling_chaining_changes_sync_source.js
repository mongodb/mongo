/**
 * Tests that setting settings.chainingAllowed to false should cause a node to change its sync
 * source if it isn't already syncing from the primary.
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");  // For syncFrom and reconfig.

const replSet = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: true},
    nodeOptions: {setParameter: {writePeriodicNoops: true}}
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const secondaries = replSet.getSecondaries();
const secondary = secondaries[0];

jsTestLog("Make sure one secondary is syncing from the other secondary");

syncFrom(secondary, secondaries[1], replSet);

jsTestLog("Reconfigure the set to disable chaining");

const newConfig = replSet.getReplSetConfigFromNode();
newConfig.version++;
newConfig.settings.chainingAllowed = false;
reconfig(replSet, newConfig);

assert.soon(() => isConfigCommitted(replSet.getPrimary()));
replSet.waitForConfigReplication(replSet.getPrimary());
replSet.awaitReplication();

jsTestLog("Do a write so that the secondary will re-evaluate its sync source");

assert.commandWorked(primary.getDB("test").foo.insert({x: 1}));
replSet.awaitSyncSource(secondary, primary);

replSet.stopSet();
})();
