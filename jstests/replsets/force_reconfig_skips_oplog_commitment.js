/**
 * Verify that a force replica set reconfig skips the oplog commitment check. The force reconfig
 * should succeed even though oplog entries committed in the previous config are not committed in
 * the current config.
 */

(function() {
"use strict";
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");  // For reconnect.

const dbName = "test";
const collName = "coll";
// Make the secondary unelectable.
let rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const coll = primary.getDB(dbName)[collName];

// This makes the test run faster.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: 'setSmallOplogGetMoreMaxTimeMS', mode: 'alwaysOn'}));

// Create collection.
assert.commandWorked(coll.insert({}));
rst.awaitReplication();

// Stop replication on the secondary.
stopServerReplication(secondary);

// Reconfig down to a 1 node replica set.
const origConfig = rst.getReplSetConfigFromNode();
let C1 = Object.assign({}, origConfig);
C1.members = C1.members.slice(0, 1);  // Remove the second node.
C1.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: C1}));

// Make sure we can connect to the secondary after it was REMOVED.
assert.soonNoExcept(function() {
    let res = secondary.adminCommand({replSetGetStatus: 1});
    assert.commandFailedWithCode(res, ErrorCodes.InvalidReplicaSetConfig);
    return true;
}, () => tojson(secondary.adminCommand({replSetGetStatus: 1})));
reconnect(secondary);

jsTestLog("Test that force reconfig skips oplog commitment.");
let C2 = Object.assign({}, origConfig);

jsTestLog("Do a write on primary and commit it in the current config.");
assert.commandWorked(coll.insert({x: 1}, {writeConcern: {w: "majority"}}));

jsTestLog("Reconfig to add the secondary back in.");

// As this force reconfig will skip the oplog commitment safety check,
// it should succeed even though the last committed op on C1 has not been committed on C2.
assert.commandWorked(primary.adminCommand({replSetReconfig: C2, force: true}));
const C3 = primary.getDB("local").system.replset.findOne();
// Run another force reconfig to verify the pre-condition check is also skipped
assert.commandWorked(primary.adminCommand({replSetReconfig: C3, force: true}));

restartServerReplication(secondary);
rst.awaitNodesAgreeOnConfigVersion();

rst.stopSet();
}());
