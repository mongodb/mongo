/**
 * Verify that a force replica set reconfig skips the config replication check.
 * The force reconfig should succeed even though the previous config has not
 * committed across a majority of nodes.
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");

const replTest = new ReplSetTest({nodes: 2, useBridge: true});
const nodes = replTest.startSet();
// Initiating with a high election timeout prevents unnecessary elections and also prevents
// the primary from stepping down if it cannot communicate with the secondary.
replTest.initiateWithHighElectionTimeout();
const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

jsTestLog("Test force reconfig skips config replication against primary");
// Disconnect the secondary from the primary.
secondary.disconnect(primary);

const C1 = primary.getDB("local").system.replset.findOne();
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: C1, force: true}));
const C2 = primary.getDB("local").system.replset.findOne();
// As this force reconfig will skip the config replication safety check,
// it should succeed even though C1 has not been replicated to a majority.
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: C2, force: true}));
secondary.reconnect(primary);

// Wait until the config has propagated to the secondary.
assert.soon(function() {
    const res = primary.adminCommand({replSetGetStatus: 1});
    return res.members[1].configVersion === replTest.getReplSetConfigFromNode().version;
});

jsTestLog("Test force reconfig skips config replication against secondary");
// Disconnect the secondary from the primary.
secondary.disconnect(primary);

const C3 = secondary.getDB("local").system.replset.findOne();
assert.commandWorked(secondary.getDB("admin").runCommand({replSetReconfig: C3, force: true}));
// As this force reconfig will skip the config replication safety check,
// it should succeed even though C1 has not been replicated to a majority.
const C4 = secondary.getDB("local").system.replset.findOne();
assert.commandWorked(secondary.getDB("admin").runCommand({replSetReconfig: C4, force: true}));
secondary.reconnect(primary);

replTest.awaitNodesAgreeOnConfigVersion();
replTest.stopSet();
}());
