/**
 * Tests that the freezeTimeout election reason counter in serverStatus is incremented in a single
 * node replica set both after a freeze timeout and after a stepdown timeout expires.
 */
(function() {
"use strict";
load('jstests/replsets/libs/election_metrics.js');

jsTestLog('1: initialize single node replica set');
const replSet = new ReplSetTest({name: 'freeze_timeout', nodes: 1});
const nodes = replSet.startSet();
const config = replSet.getReplSetConfig();
replSet.initiate(config);
replSet.awaitReplication();
let primary = replSet.getPrimary();
const initialPrimaryStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));

jsTestLog('2: step down primary');
assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 10, force: 1}));

jsTestLog('3: wait for stepped down node to become primary again');
primary = replSet.getPrimary();

// Check that both the 'called' and 'successful' fields of the 'freezeTimeout' election reason
// counter have been incremented in serverStatus. When a stepdown timeout expires in a single
// node replica set, an election is called for the same reason as is used when a freeze timeout
// expires.
let newPrimaryStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
verifyServerStatusElectionReasonCounterChange(
    initialPrimaryStatus.electionMetrics, newPrimaryStatus.electionMetrics, "freezeTimeout", 1);

jsTestLog('4: step down primary again');
assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 10, force: 1}));

jsTestLog('5: freeze stepped down primary for 30 seconds');
primary.getDB("admin").runCommand({replSetFreeze: 30});
sleep(1000);

jsTestLog('6: unfreeze stepped down primary after waiting for 1 second');
primary.getDB("admin").runCommand({replSetFreeze: 0});

jsTestLog('7: wait for unfrozen node to become primary again');
primary = replSet.getPrimary();

// Check that both the 'called' and 'successful' fields of the 'freezeTimeout' election reason
// counter have been incremented again in serverStatus.
newPrimaryStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
verifyServerStatusElectionReasonCounterChange(
    initialPrimaryStatus.electionMetrics, newPrimaryStatus.electionMetrics, "freezeTimeout", 2);

replSet.stopSet();
})();
