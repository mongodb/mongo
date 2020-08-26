// Test that certain operations fail in recovery mode.

(function() {
"use strict";

// Replica set testing API
// Create a new replica set test. Specify set name and the number of nodes you want.
var replTest = new ReplSetTest({name: 'testSet', nodes: 3});

// call startSet() to start each mongod in the replica set
// this returns a list of nodes
var nodes = replTest.startSet();

// Call initiate() to send the replSetInitiate command
// This will wait for initiation
replTest.initiate();

// Call getPrimary to return a reference to the node that's been
// elected primary.
var primary = replTest.getPrimary();

// save some records
var len = 100;
for (var i = 0; i < len; ++i) {
    primary.getDB("foo").foo.save({a: i});
}

// This method will check the oplogs of the primary
// and secondaries in the set and wait until the change has replicated.
// replTest.awaitReplication();

var secondaries = replTest.getSecondaries();
assert.eq(2, secondaries.length, "Expected 2 secondaries but length was " + secondaries.length);

secondaries.forEach(function(secondary) {
    // put secondary into maintenance (recovery) mode
    assert.commandWorked(secondary.getDB("foo").adminCommand({replSetMaintenance: 1}));

    var stats = secondary.getDB("foo").adminCommand({replSetGetStatus: 1});
    assert.eq(stats.myState, 3, "Secondary should be in recovering state.");

    print("count should fail in recovering state...");
    secondary.slaveOk = true;
    assert.commandFailed(secondary.getDB("foo").runCommand({count: "foo"}));

    // unset maintenance mode when done
    assert.commandWorked(secondary.getDB("foo").adminCommand({replSetMaintenance: 0}));
});

// Shut down the set and finish the test.
replTest.stopSet();
}());
