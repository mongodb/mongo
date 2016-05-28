load("jstests/replsets/rslib.js");

doTest = function(signal) {

    // Test replication with write concern.

    // Replica set testing API
    // Create a new replica set test. Specify set name and the number of nodes you want.
    var replTest = new ReplSetTest({name: 'testSet', nodes: 3, oplogSize: 5});

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    var nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    var testDB = "repl-test";

    // Call getPrimary to return a reference to the node that's been
    // elected master.
    var master = replTest.getPrimary();

    // Wait for replication to a single node
    master.getDB(testDB).bar.insert({n: 1});

    // Wait for states to become PRI,SEC,SEC
    waitForAllMembers(master.getDB(testDB));

    var slaves = replTest.liveNodes.slaves;
    slaves.forEach(function(slave) {
        slave.setSlaveOk();
    });

    // Test write concern with multiple inserts.
    print("\n\nreplset2.js **** Try inserting a multiple records -- first insert ****");

    printjson(master.getDB("admin").runCommand("replSetGetStatus"));

    var bulk = master.getDB(testDB).foo.initializeUnorderedBulkOp();
    bulk.insert({n: 1});
    bulk.insert({n: 2});
    bulk.insert({n: 3});

    print("\nreplset2.js **** TEMP 1 ****");

    printjson(master.getDB("admin").runCommand("replSetGetStatus"));

    assert.writeOK(bulk.execute({w: 3, wtimeout: 25000}));

    print("replset2.js **** TEMP 1a ****");

    m1 = master.getDB(testDB).foo.findOne({n: 1});
    printjson(m1);
    assert(m1['n'] == 1, "replset2.js Failed to save to master on multiple inserts");

    print("replset2.js **** TEMP 1b ****");

    var s0 = slaves[0].getDB(testDB).foo.findOne({n: 1});
    assert(s0['n'] == 1, "replset2.js Failed to replicate to slave 0 on multiple inserts");

    var s1 = slaves[1].getDB(testDB).foo.findOne({n: 1});
    assert(s1['n'] == 1, "replset2.js Failed to replicate to slave 1 on multiple inserts");

    // Test write concern with a simple insert
    print("replset2.js **** Try inserting a single record ****");
    master.getDB(testDB).dropDatabase();
    var options = {writeConcern: {w: 3, wtimeout: 10000}};
    assert.writeOK(master.getDB(testDB).foo.insert({n: 1}, options));

    m1 = master.getDB(testDB).foo.findOne({n: 1});
    printjson(m1);
    assert(m1['n'] == 1, "replset2.js Failed to save to master");

    s0 = slaves[0].getDB(testDB).foo.findOne({n: 1});
    assert(s0['n'] == 1, "replset2.js Failed to replicate to slave 0");

    s1 = slaves[1].getDB(testDB).foo.findOne({n: 1});
    assert(s1['n'] == 1, "replset2.js Failed to replicate to slave 1");

    print("replset2.js **** Try inserting many records ****");
    try {
        var bigData = new Array(2000).toString();
        bulk = master.getDB(testDB).baz.initializeUnorderedBulkOp();
        for (var n = 0; n < 1000; n++) {
            bulk.insert({n: n, data: bigData});
        }
        assert.writeOK(bulk.execute({w: 3, wtimeout: 60000}));

        print("replset2.js **** V1 ");

        var verifyReplication = function(nodeName, collection) {
            data = collection.findOne({n: 1});
            assert(data['n'] == 1, "replset2.js Failed to save to " + nodeName);
            data = collection.findOne({n: 999});
            assert(data['n'] == 999, "replset2.js Failed to save to " + nodeName);
        };

        print("replset2.js **** V2 ");

        verifyReplication("master", master.getDB(testDB).baz);
        verifyReplication("slave 0", slaves[0].getDB(testDB).baz);
        verifyReplication("slave 1", slaves[1].getDB(testDB).baz);
    } catch (e) {
        var errstr = "ERROR: " + e;
        errstr += "\nMaster oplog findOne:\n";
        errstr +=
            tojson(master.getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
        errstr += "\nSlave 0 oplog findOne:\n";
        errstr +=
            tojson(slaves[0].getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
        errstr += "\nSlave 1 oplog findOne:\n";
        errstr +=
            tojson(slaves[1].getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
        assert(false, errstr);
    }

    replTest.stopSet(signal);
};

doTest(15);

print("\nreplset2.js SUCCESS\n");
