// Test that setting maxSyncSourceLagSecs causes the set to change sync target
//
// This test requires the fsync command to ensure members experience a delay.
// @tags: [requires_fsync]
(function() {
"use strict";
load("jstests/replsets/rslib.js");

var name = "maxSyncSourceLagSecs";
var replTest = new ReplSetTest({
    name: name,
    nodes: [
        {rsConfig: {priority: 3}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}, setParameter: 'maxSyncSourceLagSecs=3'},
    ],
    oplogSize: 5,
});
var nodes = replTest.nodeList();
replTest.startSet();
replTest.initiate();
replTest.awaitNodesAgreeOnPrimary();
var primary = replTest.getPrimary();
var secondaries = replTest.getSecondaries();

// The default WC is majority and stopServerReplication could prevent satisfying any majority
// writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

replTest.awaitReplication();

syncFrom(secondaries[0], primary, replTest);
syncFrom(secondaries[1], primary, replTest);
primary.getDB("foo").bar.save({a: 1});
replTest.awaitReplication();

jsTestLog("Setting sync target of secondary 2 to secondary 1");
syncFrom(secondaries[1], secondaries[0], replTest);
printjson(replTest.status());

// need to put at least maxSyncSourceLagSecs b/w first op and subsequent ops
// so that the shouldChangeSyncSource logic goes into effect
sleep(4000);

jsTestLog(
    "Lock secondary 1 and add some docs. Force sync target for secondary 2 to change to primary");
assert.commandWorked(secondaries[0].getDB("admin").runCommand({fsync: 1, lock: 1}));

assert.soon(function() {
    primary.getDB("foo").bar.insert({a: 2});
    var res = secondaries[1].getDB("admin").runCommand({"replSetGetStatus": 1});
    return res.syncSourceHost === primary.name;
}, "sync target not changed back to primary", 100 * 1000, 2 * 1000);
printjson(replTest.status());

assert.soon(function() {
    return (secondaries[1].getDB("foo").bar.count({a: 1}) > 0 &&
            secondaries[1].getDB("foo").bar.count({a: 2}) > 0);
}, "secondary should have caught up after syncing to primary.");

assert.commandWorked(secondaries[0].getDB("admin").fsyncUnlock());
replTest.stopSet();
}());
