/**
 * Test that trying to add a node via safe reconfig fails if the node is already in an active
 * replica set. We have two replica sets, A and B, where A has two nodes, A_0 and A_1, and B has one
 * node, B_0. Adding B_0 to replica set A should fail on detecting an inconsistent replica set ID in
 * the heartbeat response metadata from B_0.
 * @tags: [requires_fcv_49]
 */

(function() {
'use strict';

const name = jsTestName();
const replSetA = new ReplSetTest({
    name,
    nodes: [
        {rsConfig: {_id: 10}},
        {rsConfig: {_id: 11, priority: 0}},
    ],
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({replication: 2})}}
});
replSetA.startSet({dbpath: "$set-A-$node"});
replSetA.initiate();

const replSetB = new ReplSetTest({
    name,
    nodes: [
        {rsConfig: {_id: 20}},
    ],
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({replication: 2})}}
});
replSetB.startSet({dbpath: "$set-B-$node"});
replSetB.initiate();

const primaryA = replSetA.getPrimary();
const primaryB = replSetB.getPrimary();
assert.commandWorked(primaryA.getDB('foo').bar.insert({a: 1}));
assert.commandWorked(primaryB.getDB('foo').bar.insert({b: 1}));
jsTestLog('Before merging: primary A = ' + primaryA.host + '; primary B = ' + primaryB.host);

let configA = assert.commandWorked(primaryA.adminCommand({replSetGetConfig: 1})).config;
let configB = assert.commandWorked(primaryB.adminCommand({replSetGetConfig: 1})).config;
assert(configA.settings.replicaSetId instanceof ObjectId);
assert(configB.settings.replicaSetId instanceof ObjectId);
jsTestLog('Replica set A ID = ' + configA.settings.replicaSetId);
jsTestLog('Replica set B ID = ' + configB.settings.replicaSetId);
assert.neq(configA.settings.replicaSetId, configB.settings.replicaSetId);

// Increment the config version first on this node so that its version on the next reconfig will
// be higher than B's.
configA.version++;
assert.commandWorked(primaryA.adminCommand({replSetReconfig: configA}));

jsTestLog("Adding replica set B's primary " + primaryB.host + " to replica set A's config");
configA.version++;
configA.members.push({_id: 12, host: primaryB.host});
const reconfigResult =
    assert.commandFailedWithCode(primaryA.adminCommand({replSetReconfig: configA}),
                                 ErrorCodes.NewReplicaSetConfigurationIncompatible);
const msgA = 'Our replica set ID did not match that of our request target, replSetId: ' +
    configA.settings.replicaSetId + ', requestTarget: ' + primaryB.host +
    ', requestTargetReplSetId: ' + configB.settings.replicaSetId;
assert.neq(-1, reconfigResult.errmsg.indexOf(msgA));

const newPrimaryA = replSetA.getPrimary();
const newPrimaryB = replSetB.getPrimary();
jsTestLog('After merging: primary A = ' + newPrimaryA.host + '; primary B = ' + newPrimaryB.host);
assert.eq(primaryA, newPrimaryA);
assert.eq(primaryB, newPrimaryB);

// Mismatch replica set IDs in heartbeat responses should be logged.
const msgB = "replica set IDs do not match, ours: " + configB.settings.replicaSetId +
    "; remote node's: " + configA.settings.replicaSetId;
checkLog.contains(primaryB, msgB);

const statusA = assert.commandWorked(primaryA.adminCommand({replSetGetStatus: 1}));
const statusB = assert.commandWorked(primaryB.adminCommand({replSetGetStatus: 1}));
jsTestLog('After merging: replica set status A = ' + tojson(statusA));
jsTestLog('After merging: replica set status B = ' + tojson(statusB));

// Replica set A's config should remain unchanged due to failed replSetReconfig command.
assert.eq(2, statusA.members.length);
assert.eq(10, statusA.members[0]._id);
assert.eq(primaryA.host, statusA.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusA.members[0].state);
assert.eq(11, statusA.members[1]._id);

// Replica set B's config should remain unchanged.
assert.eq(1, statusB.members.length);
assert.eq(20, statusB.members[0]._id);
assert.eq(primaryB.host, statusB.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusB.members[0].state);

replSetB.stopSet();
replSetA.stopSet();
})();
