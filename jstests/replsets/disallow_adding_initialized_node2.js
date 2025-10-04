// If a node is already in an active replica set, it is not possible to add this node to another
// replica set.
// Initialize two replica sets A and B with the same name: A_0, A_1; B_0
// Stop B_0.
// Add B_0 to the replica set A.
// Start B_0.
// B_0 should show up in A's replica set status as DOWN.

import {ReplSetTest} from "jstests/libs/replsettest.js";

// This test requires users to persist across a restart.
// @tags: [requires_persistence]

let name = "disallow_adding_initialized_node2";
let replSetA = new ReplSetTest({
    name: name,
    nodes: [{rsConfig: {_id: 10}}, {rsConfig: {_id: 11, priority: 0}}],
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({replication: 2})}},
});
replSetA.startSet({dbpath: "$set-A-$node"});
replSetA.initiate();

let replSetB = new ReplSetTest({
    name: name,
    nodes: [{rsConfig: {_id: 20}}],
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({replication: 2})}},
});
replSetB.startSet({dbpath: "$set-B-$node"});
replSetB.initiate();

let primaryA = replSetA.getPrimary();
let primaryB = replSetB.getPrimary();
jsTestLog("Before merging: primary A = " + primaryA.host + "; primary B = " + primaryB.host);

let configA = assert.commandWorked(primaryA.adminCommand({replSetGetConfig: 1})).config;
let configB = assert.commandWorked(primaryB.adminCommand({replSetGetConfig: 1})).config;
assert(configA.settings.replicaSetId instanceof ObjectId);
assert(configB.settings.replicaSetId instanceof ObjectId);
jsTestLog("Replica set A ID = " + configA.settings.replicaSetId);
jsTestLog("Replica set B ID = " + configB.settings.replicaSetId);
assert.neq(configA.settings.replicaSetId, configB.settings.replicaSetId);

jsTestLog("Stopping B's primary " + primaryB.host);
replSetB.stop(0);

jsTestLog("Adding replica set B's primary " + primaryB.host + " to replica set A's config");
configA.version++;
configA.members.push({_id: 12, host: primaryB.host});
// Use "force" reconfig to increase the config of replica set A by a large number, so that replica
// set B will try to fetch the config with a higher version on hearing it via heartbeats.
assert.commandWorked(primaryA.adminCommand({replSetReconfig: configA, force: true}));

jsTestLog("Restarting B's primary " + primaryB.host);
primaryB = replSetB.start(0, {dbpath: "$set-B-$node", restart: true});

let newPrimaryA = replSetA.getPrimary();
let newPrimaryB = replSetB.getPrimary();
jsTestLog("After merging: primary A = " + newPrimaryA.host + "; primary B = " + newPrimaryB.host);
assert.eq(primaryA, newPrimaryA);
assert.eq(primaryB, newPrimaryB);

// Mismatch replica set IDs in heartbeat responses should be logged.
let msgA =
    "replica set IDs do not match, ours: " +
    configA.settings.replicaSetId +
    "; remote node's: " +
    configB.settings.replicaSetId;
let msgB =
    "replica set IDs do not match, ours: " +
    configB.settings.replicaSetId +
    "; remote node's: " +
    configA.settings.replicaSetId;
checkLog.contains(primaryA, msgA);
checkLog.contains(primaryB, msgB);

let statusA = assert.commandWorked(primaryA.adminCommand({replSetGetStatus: 1}));
let statusB = assert.commandWorked(primaryB.adminCommand({replSetGetStatus: 1}));
jsTestLog("After merging: replica set status A = " + tojson(statusA));
jsTestLog("After merging: replica set status B = " + tojson(statusB));

// B's primary should show up in A's status as DOWN.
assert.eq(3, statusA.members.length);
assert.eq(10, statusA.members[0]._id);
assert.eq(primaryA.host, statusA.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusA.members[0].state);
assert.eq(12, statusA.members[2]._id);
assert.eq(primaryB.host, statusA.members[2].name);
assert.eq(ReplSetTest.State.DOWN, statusA.members[2].state);

// Replica set B's config should remain unchanged.
assert.eq(1, statusB.members.length);
assert.eq(20, statusB.members[0]._id);
assert.eq(primaryB.host, statusB.members[0].name);
assert.eq(ReplSetTest.State.PRIMARY, statusB.members[0].state);

replSetB.stopSet();
replSetA.stopSet();
