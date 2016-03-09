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
    // elected master.
    var master = replTest.getPrimary();

    // save some records
    var len = 100;
    for (var i = 0; i < len; ++i) {
        master.getDB("foo").foo.save({a: i});
    }

    // This method will check the oplogs of the master
    // and slaves in the set and wait until the change has replicated.
    // replTest.awaitReplication();

    var slaves = replTest.liveNodes.slaves;
    assert.eq(2, slaves.length, "Expected 2 slaves but length was " + slaves.length);

    slaves.forEach(function(slave) {
        // put slave into maintenance (recovery) mode
        slave.getDB("foo").adminCommand({replSetMaintenance: 1});

        var stats = slave.getDB("foo").adminCommand({replSetGetStatus: 1});
        assert.eq(stats.myState, 3, "Slave should be in recovering state.");

        print("group should fail in recovering state...");
        slave.slaveOk = true;
        assert.commandFailed(slave.getDB("foo").foo.runCommand({
            group: {
                ns: "foo",
                initial: {n: 0},
                $reduce: function(obj, out) {
                    out.n++;
                }
            }
        }));

        print("count should fail in recovering state...");
        slave.slaveOk = true;
        assert.commandFailed(slave.getDB("foo").runCommand({count: "foo"}));
    });

    // Shut down the set and finish the test.
    replTest.stopSet();
}());
