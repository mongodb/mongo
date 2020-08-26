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
    // elected primary.
    var primary = replTest.getPrimary();

    // Wait for replication to a single node
    primary.getDB(testDB).bar.insert({n: 1});

    // Wait for states to become PRI,SEC,SEC
    waitForAllMembers(primary.getDB(testDB));

    var secondaries = replTest.getSecondaries();
    secondaries.forEach(function(secondary) {
        secondary.setSlaveOk();
    });

    // Test write concern with multiple inserts.
    print("\n\nreplset2.js **** Try inserting a multiple records -- first insert ****");

    printjson(primary.getDB("admin").runCommand("replSetGetStatus"));

    var bulk = primary.getDB(testDB).foo.initializeUnorderedBulkOp();
    bulk.insert({n: 1});
    bulk.insert({n: 2});
    bulk.insert({n: 3});

    print("\nreplset2.js **** TEMP 1 ****");

    printjson(primary.getDB("admin").runCommand("replSetGetStatus"));

    assert.commandWorked(bulk.execute({w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}));

    print("replset2.js **** TEMP 1a ****");

    m1 = primary.getDB(testDB).foo.findOne({n: 1});
    printjson(m1);
    assert(m1['n'] == 1, "replset2.js Failed to save to primary on multiple inserts");

    print("replset2.js **** TEMP 1b ****");

    var s0 = secondaries[0].getDB(testDB).foo.findOne({n: 1});
    assert(s0['n'] == 1, "replset2.js Failed to replicate to secondary 0 on multiple inserts");

    var s1 = secondaries[1].getDB(testDB).foo.findOne({n: 1});
    assert(s1['n'] == 1, "replset2.js Failed to replicate to secondary 1 on multiple inserts");

    // Test write concern with a simple insert
    print("replset2.js **** Try inserting a single record ****");
    primary.getDB(testDB).dropDatabase();
    var options = {writeConcern: {w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}};
    assert.commandWorked(primary.getDB(testDB).foo.insert({n: 1}, options));

    m1 = primary.getDB(testDB).foo.findOne({n: 1});
    printjson(m1);
    assert(m1['n'] == 1, "replset2.js Failed to save to primary");

    s0 = secondaries[0].getDB(testDB).foo.findOne({n: 1});
    assert(s0['n'] == 1, "replset2.js Failed to replicate to secondary 0");

    s1 = secondaries[1].getDB(testDB).foo.findOne({n: 1});
    assert(s1['n'] == 1, "replset2.js Failed to replicate to secondary 1");

    print("replset2.js **** Try inserting many records ****");
    try {
        var bigData = new Array(2000).toString();
        bulk = primary.getDB(testDB).baz.initializeUnorderedBulkOp();
        for (var n = 0; n < 1000; n++) {
            bulk.insert({n: n, data: bigData});
        }
        assert.commandWorked(bulk.execute({w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}));

        print("replset2.js **** V1 ");

        var verifyReplication = function(nodeName, collection) {
            data = collection.findOne({n: 1});
            assert(data['n'] == 1, "replset2.js Failed to save to " + nodeName);
            data = collection.findOne({n: 999});
            assert(data['n'] == 999, "replset2.js Failed to save to " + nodeName);
        };

        print("replset2.js **** V2 ");

        verifyReplication("primary", primary.getDB(testDB).baz);
        verifyReplication("secondary 0", secondaries[0].getDB(testDB).baz);
        verifyReplication("secondary 1", secondaries[1].getDB(testDB).baz);
    } catch (e) {
        var errstr = "ERROR: " + e;
        errstr += "\nMaster oplog findOne:\n";
        errstr +=
            tojson(primary.getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
        errstr += "\nSecondary 0 oplog findOne:\n";
        errstr += tojson(
            secondaries[0].getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
        errstr += "\nSecondary 1 oplog findOne:\n";
        errstr += tojson(
            secondaries[1].getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
        assert(false, errstr);
    }

    replTest.stopSet(signal);
};

doTest(15);

print("\nreplset2.js SUCCESS\n");
