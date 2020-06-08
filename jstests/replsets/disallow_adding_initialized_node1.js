// If a node is already in an active replica set, it is not possible to add this node to another
// replica set.
// Initialize two replica sets A and B with the same name: A_0; B_0
// Add B_0 to the replica set A. This operation should fail on replica set A should fail on
// detecting an inconsistent replica set ID in the heartbeat response metadata from B_0.

(function() {
'use strict';

var name = 'disallow_adding_initialized_node1';
var replSetA = new ReplSetTest({
    name: name,
    nodes: [
        {rsConfig: {_id: 10}},
    ]
});
replSetA.startSet({dbpath: "$set-A-$node"});
replSetA.initiate();

var replSetB = new ReplSetTest({
    name: name,
    nodes: [
        {rsConfig: {_id: 20}},
    ]
});
replSetB.startSet({dbpath: "$set-B-$node"});
replSetB.initiate();

var primaryA = replSetA.getPrimary();
var primaryB = replSetB.getPrimary();
assert.commandWorked(primaryA.getDB('foo').bar.insert({a: 1}));
assert.commandWorked(primaryB.getDB('foo').bar.insert({b: 1}));
jsTestLog('Before merging: primary A = ' + primaryA.host + '; primary B = ' + primaryB.host);

var configA = assert.commandWorked(primaryA.adminCommand({replSetGetConfig: 1})).config;
var configB = assert.commandWorked(primaryB.adminCommand({replSetGetConfig: 1})).config;
assert(configA.settings.replicaSetId instanceof ObjectId);
assert(configB.settings.replicaSetId instanceof ObjectId);
jsTestLog('Replica set A ID = ' + configA.settings.replicaSetId);
jsTestLog('Replica set B ID = ' + configB.settings.replicaSetId);
assert.neq(configA.settings.replicaSetId, configB.settings.replicaSetId);

// Increment the config version first on this node so that its version on the next reconfig will
// be higher than B's.
configA.version++;
assert.commandWorked(primaryA.adminCommand({replSetReconfig: configA}));

// We add B's primary with 0 votes so no 'newlyAdded' field is added, so a user initiated reconfig
// to give it 1 vote will fail, which is what we'd like to test. Since B's primary has 0 votes,
// it is not considered part of the reconfig quorum and does not block the reconfig from succeeding.
jsTestLog("Adding replica set B's primary " + primaryB.host +
          " to replica set A's config with 0 votes");
configA.version++;
configA.members.push({_id: 11, host: primaryB.host, votes: 0, priority: 0});
assert.commandWorked(primaryA.adminCommand({replSetReconfig: configA}));

// Wait for primary A to report primary B down. B should reject all heartbeats from A due to a
// replset name mismatch, leading A to consider it down.
assert.soon(function() {
    const statusA = assert.commandWorked(primaryA.adminCommand({replSetGetStatus: 1}));
    if (statusA.members.length !== 2) {
        return false;
    }
    return statusA.members[1].state === ReplSetTest.State.DOWN;
});

// Confirm that each set still has the correct primary.
let newPrimaryA = replSetA.getPrimary();
let newPrimaryB = replSetB.getPrimary();
jsTestLog('After merging with 0 votes: primary A = ' + newPrimaryA.host +
          '; primary B = ' + newPrimaryB.host);
assert.eq(primaryA, newPrimaryA);
assert.eq(primaryB, newPrimaryB);

// Replica set A's config should include primary B and consider it DOWN.
let statusA = assert.commandWorked(primaryA.adminCommand({replSetGetStatus: 1}));
jsTestLog('After merging with 0 votes: replica set status A = ' + tojson(statusA));
assert.eq(2, statusA.members.length);
assert.eq(10, statusA.members[0]._id);
assert.eq(primaryA.host, statusA.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusA.members[0].state);
assert.eq(11, statusA.members[1]._id);
assert.eq(primaryB.host, statusA.members[1].name);
assert.eq(ReplSetTest.State.DOWN, statusA.members[1].state);

// Replica set B's config should remain unchanged.
let statusB = assert.commandWorked(primaryB.adminCommand({replSetGetStatus: 1}));
jsTestLog('After merging with 0 votes: replica set status B = ' + tojson(statusB));
assert.eq(1, statusB.members.length);
assert.eq(20, statusB.members[0]._id);
assert.eq(primaryB.host, statusB.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusB.members[0].state);

// This reconfig should fail since B's primary is now part of the reconfig quorum and should reject
// it.
jsTestLog("Giving replica set B's primary " + primaryB.host + " 1 vote in replica set A's config");
configA.version++;
configA.members[1].votes = 1;
var reconfigResult =
    assert.commandFailedWithCode(primaryA.adminCommand({replSetReconfig: configA}),
                                 ErrorCodes.NewReplicaSetConfigurationIncompatible);
var msgA = 'Our replica set ID did not match that of our request target, replSetId: ' +
    configA.settings.replicaSetId + ', requestTarget: ' + primaryB.host +
    ', requestTargetReplSetId: ' + configB.settings.replicaSetId;
assert.neq(-1, reconfigResult.errmsg.indexOf(msgA));

newPrimaryA = replSetA.getPrimary();
newPrimaryB = replSetB.getPrimary();
jsTestLog('After merging: primary A = ' + newPrimaryA.host + '; primary B = ' + newPrimaryB.host);
assert.eq(primaryA, newPrimaryA);
assert.eq(primaryB, newPrimaryB);

// Mismatch replica set IDs in heartbeat responses should be logged.
var msgB = "replica set IDs do not match, ours: " + configB.settings.replicaSetId +
    "; remote node's: " + configA.settings.replicaSetId;
checkLog.contains(primaryB, msgB);

// Confirm primary B is still DOWN.
statusA = assert.commandWorked(primaryA.adminCommand({replSetGetStatus: 1}));
jsTestLog('After merging: replica set status A = ' + tojson(statusA));
assert.eq(2, statusA.members.length);
assert.eq(10, statusA.members[0]._id);
assert.eq(primaryA.host, statusA.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusA.members[0].state);
assert.eq(11, statusA.members[1]._id);
assert.eq(primaryB.host, statusA.members[1].name);
assert.eq(ReplSetTest.State.DOWN, statusA.members[1].state);

// Replica set B's config should remain unchanged.
statusB = assert.commandWorked(primaryB.adminCommand({replSetGetStatus: 1}));
jsTestLog('After merging: replica set status B = ' + tojson(statusB));
assert.eq(1, statusB.members.length);
assert.eq(20, statusB.members[0]._id);
assert.eq(primaryB.host, statusB.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusB.members[0].state);

assert.eq(1, primaryA.getDB('foo').bar.find({a: 1}).itcount());
assert.eq(0, primaryA.getDB('foo').bar.find({b: 1}).itcount());
assert.eq(0, primaryB.getDB('foo').bar.find({a: 1}).itcount());
assert.eq(1, primaryB.getDB('foo').bar.find({b: 1}).itcount());

replSetB.stopSet();
replSetA.stopSet();
})();
