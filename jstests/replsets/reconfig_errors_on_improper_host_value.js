/**
 * ReplSetReconfig should error out if any of the members in the replica set have a
 * 'host' field that contains a connection string.
 */

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 3});

rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

// Achieve steady state replication.
assert.commandWorked(primary.getDB(jsTestName())["test"].insert({x: 1}, {writeConcern: {w: 3}}));
rst.awaitReplication();

// Store proper host values.
let config = primary.getDB("local").system.replset.findOne();
const memberZeroHostValue = config.members[0].host;
const memberOneHostValue = config.members[1].host;
const memberTwoHostValue = config.members[2].host;

// Populate host field with connection string and expect a reconfig failure.
config.members[2].host = "mongodb://host/?replicaSet=rs";
config.version++;
assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                             ErrorCodes.InvalidReplicaSetConfig,
                             "Reconfig Should Fail");

// Verify that using connection string for all members' host fields fails.
config.members[1].host = "mongodb://host/?replicaSet=rs";
config.members[0].host = "mongodb://host/?replicaSet=rs";
config.version++;
assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                             ErrorCodes.InvalidReplicaSetConfig,
                             "Reconfig Should Fail");

// Sanity check that resetting member's host fields to proper host value makes reconfig work.
config.members[0].host = memberZeroHostValue;
config.members[1].host = memberOneHostValue;
config.members[2].host = memberTwoHostValue;
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
rst.stopSet();
})();
