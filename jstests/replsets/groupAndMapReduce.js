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
    // elected master.
    var master = replTest.getPrimary();

    // save some records
    var len = 100;
    for (var i = 0; i < len; ++i) {
        master.getDB("foo").foo.save({a: i});
    }

    waitForAllMembers(master.getDB("foo"));
    // This method will check the oplogs of the master
    // and slaves in the set and wait until the change has replicated.
    replTest.awaitReplication();

    slaves = replTest.liveNodes.slaves;
    assert(slaves.length == 2, "Expected 2 slaves but length was " + slaves.length);
    slaves.forEach(function(slave) {
        // try to read from slave
        slave.slaveOk = true;
        var count = slave.getDB("foo").foo.find().itcount();
        printjson(count);
        assert.eq(len, count, "slave count wrong: " + slave);

        print("Doing a findOne to verify we can get a row");
        var one = slave.getDB("foo").foo.findOne();
        printjson(one);

        //        stats = slave.getDB("foo").adminCommand({replSetGetStatus:1});
        //        printjson(stats);

        print("Calling group() with slaveOk=true, must succeed");
        slave.slaveOk = true;
        count = slave.getDB("foo").foo.group({
            initial: {n: 0},
            reduce: function(obj, out) {
                out.n++;
            }
        });
        printjson(count);
        assert.eq(len, count[0].n, "slave group count wrong: " + slave);

        print("Calling group() with slaveOk=false, must fail");
        slave.slaveOk = false;
        try {
            count = slave.getDB("foo").foo.group({
                initial: {n: 0},
                reduce: function(obj, out) {
                    out.n++;
                }
            });
            assert(false, "group() succeeded with slaveOk=false");
        } catch (e) {
            print("Received exception: " + e);
        }

        print("Calling inline mr() with slaveOk=true, must succeed");
        slave.slaveOk = true;
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
        slave.getDB("foo").foo.mapReduce(map, reduce, {out: {"inline": 1}});

        print("Calling mr() to collection with slaveOk=true, must fail");
        try {
            slave.getDB("foo").foo.mapReduce(map, reduce, "output");
            assert(false, "mapReduce() to collection succeeded on slave");
        } catch (e) {
            print("Received exception: " + e);
        }

        print("Calling inline mr() with slaveOk=false, must fail");
        slave.slaveOk = false;
        try {
            slave.getDB("foo").foo.mapReduce(map, reduce, {out: {"inline": 1}});
            assert(false, "mapReduce() succeeded on slave with slaveOk=false");
        } catch (e) {
            print("Received exception: " + e);
        }
        print("Calling mr() to collection with slaveOk=false, must fail");
        try {
            slave.getDB("foo").foo.mapReduce(map, reduce, "output");
            assert(false, "mapReduce() to collection succeeded on slave with slaveOk=false");
        } catch (e) {
            print("Received exception: " + e);
        }

    });

    // Shut down the set and finish the test.
    replTest.stopSet(signal);
};

doTest(15);
print("SUCCESS");
