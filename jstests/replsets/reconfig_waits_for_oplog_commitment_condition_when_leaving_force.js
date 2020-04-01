/**
 * Verify that a non force replica set reconfig waits for all oplog entries committed in the
 * previous config to be committed in the current config even if we are exiting a config that was
 * installed via a 'force' reconfig.
 *
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

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

// Force reconfig down to a 1 node replica set.
let twoNodeConfig = rst.getReplSetConfigFromNode();
let singleNodeConfig = Object.assign({}, twoNodeConfig);
singleNodeConfig.members = singleNodeConfig.members.slice(0, 1);  // Remove the second node.
singleNodeConfig.version++;

jsTestLog("Force reconfig down to a single node.");
assert.commandWorked(primary.adminCommand({replSetReconfig: singleNodeConfig, force: true}));
assert(isConfigCommitted(primary));

jsTestLog("Do a write on primary and commit it in the current config.");
assert.commandWorked(coll.insert({x: 1}, {writeConcern: {w: "majority"}}));

jsTestLog("Force reconfig to add the secondary back in.");
// We expect this to succeed but the last committed op from the previous config
// isn't committed in the current config yet.
twoNodeConfig.version = rst.getReplSetConfigFromNode().version + 1;
assert.commandWorked(primary.adminCommand({replSetReconfig: twoNodeConfig, force: true}));

// Wait until the config has propagated to the secondary and the primary has learned of it, so that
// the config replication check is satisfied.
waitForConfigReplication(primary);

// Reconfig should succeed even if we have not committed the last committed op in the current
// config because the current config is from a force reconfig.
assert.eq(isConfigCommitted(primary), false);
twoNodeConfig.version = rst.getReplSetConfigFromNode().version + 1;
assert.commandWorked(primary.adminCommand({replSetReconfig: twoNodeConfig}));

// Make sure we can connect to the secondary after it was REMOVED.
reconnect(secondary);

// Let the last committed op from the original 1 node config become committed in the current config.
restartServerReplication(secondary);
rst.awaitReplication();
assert.soon(() => isConfigCommitted(primary));

// Now that we can commit the op in the new config, reconfig should succeed.
twoNodeConfig.version = rst.getReplSetConfigFromNode().version + 1;
assert.commandWorked(primary.adminCommand({replSetReconfig: twoNodeConfig}));
assert(isConfigCommitted(primary));
rst.awaitReplication();

rst.stopSet();
}());
