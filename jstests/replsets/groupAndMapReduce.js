load("jstests/replsets/rslib.js");

doTest = function(signal) {
    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

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

    waitForAllMembers(primary.getDB("foo"));
    // This method will check the oplogs of the primary
    // and secondaries in the set and wait until the change has replicated.
    replTest.awaitReplication();

    secondaries = replTest.getSecondaries();
    assert(secondaries.length == 2, "Expected 2 secondaries but length was " + secondaries.length);
    secondaries.forEach(function(secondary) {
        // try to read from secondary
        secondary.setSecondaryOk();
        var count = secondary.getDB("foo").foo.find().itcount();
        printjson(count);
        assert.eq(len, count, "secondary count wrong: " + secondary);

        print("Doing a findOne to verify we can get a row");
        var one = secondary.getDB("foo").foo.findOne();
        printjson(one);

        print("Calling inline mr() with slaveOk=true, must succeed");
        secondary.setSecondaryOk();
        map = function() {
            emit(this.a, 1);
        };
        reduce = function(key, vals) {
            var sum = 0;
            for (var i = 0; i < vals.length; ++i) {
                sum += vals[i];
            }
            return sum;
        };
        secondary.getDB("foo").foo.mapReduce(map, reduce, {out: {"inline": 1}});

        print("Calling mr() to collection with slaveOk=true, must fail");
        try {
            secondary.getDB("foo").foo.mapReduce(map, reduce, "output");
            assert(false, "mapReduce() to collection succeeded on secondary");
        } catch (e) {
            print("Received exception: " + e);
        }

        print("Calling inline mr() with slaveOk=false, must fail");
        secondary.slaveOk = false;
        try {
            secondary.getDB("foo").foo.mapReduce(map, reduce, {out: {"inline": 1}});
            assert(false, "mapReduce() succeeded on secondary with slaveOk=false");
        } catch (e) {
            print("Received exception: " + e);
        }
        print("Calling mr() to collection with slaveOk=false, must fail");
        try {
            secondary.getDB("foo").foo.mapReduce(map, reduce, "output");
            assert(false, "mapReduce() to collection succeeded on secondary with slaveOk=false");
        } catch (e) {
            print("Received exception: " + e);
        }
    });

    // Shut down the set and finish the test.
    replTest.stopSet(signal);
};

doTest(15);
print("SUCCESS");
