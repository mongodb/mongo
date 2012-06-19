load("jstests/replsets/rslib.js");

doTest = function (signal) {

    // FAILING TEST
    // See below:

    // Test replication with getLastError

    // Replica set testing API
    // Create a new replica set test. Specify set name and the number of nodes you want.
    var replTest = new ReplSetTest({ name: 'testSet', nodes: 3, oplogSize: 5 });

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    var nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    var testDB = "repl-test";

    // Call getMaster to return a reference to the node that's been
    // elected master.
    var master = replTest.getMaster();

    // Wait for replication to a single node
    master.getDB(testDB).bar.insert({ n: 1 });

    // Wait for states to become PRI,SEC,SEC
    waitForAllMembers(master.getDB(testDB));

    var slaves = replTest.liveNodes.slaves;
    slaves.forEach(function (slave) { slave.setSlaveOk(); });

    var failed = false;
    var callGetLastError = function (w, timeout, db) {
        try {
            var result = master.getDB(db).getLastErrorObj(w, timeout);
            print("replset2.js getLastError result: " + tojson(result));
            if (result['ok'] != 1) {
                print("replset2.js FAILURE getlasterror not ok");
                failed = true;
            }
        }
        catch (e) {
            print("\nreplset2.js exception in getLastError: " + e + '\n');
            throw e;
        }
    }

    // Test getlasterror with multiple inserts
    // TEST FAILS HEREg
    print("\n\nreplset2.js **** Try inserting a multiple records -- first insert ****")

    printjson(master.getDB("admin").runCommand("replSetGetStatus"));

    master.getDB(testDB).foo.insert({ n: 1 });
    master.getDB(testDB).foo.insert({ n: 2 });
    master.getDB(testDB).foo.insert({ n: 3 });

    print("\nreplset2.js **** TEMP 1 ****")

    printjson(master.getDB("admin").runCommand("replSetGetStatus"));

    callGetLastError(3, 25000, testDB);

    print("replset2.js **** TEMP 1a ****")

    m1 = master.getDB(testDB).foo.findOne({ n: 1 });
    printjson(m1);
    assert(m1['n'] == 1, "replset2.js Failed to save to master on multiple inserts");

    print("replset2.js **** TEMP 1b ****")

    var s0 = slaves[0].getDB(testDB).foo.findOne({ n: 1 });
    assert(s0['n'] == 1, "replset2.js Failed to replicate to slave 0 on multiple inserts");

    var s1 = slaves[1].getDB(testDB).foo.findOne({ n: 1 });
    assert(s1['n'] == 1, "replset2.js Failed to replicate to slave 1 on multiple inserts");

    // Test getlasterror with a simple insert
    print("replset2.js **** Try inserting a single record ****")
    master.getDB(testDB).dropDatabase();
    master.getDB(testDB).foo.insert({ n: 1 });
    callGetLastError(3, 10000, testDB);

    m1 = master.getDB(testDB).foo.findOne({ n: 1 });
    printjson(m1);
    assert(m1['n'] == 1, "replset2.js Failed to save to master");

    s0 = slaves[0].getDB(testDB).foo.findOne({ n: 1 });
    assert(s0['n'] == 1, "replset2.js Failed to replicate to slave 0");

    s1 = slaves[1].getDB(testDB).foo.findOne({ n: 1 });
    assert(s1['n'] == 1, "replset2.js Failed to replicate to slave 1");

    // Test getlasterror with large insert
    print("replset2.js **** Try inserting many records ****")
    try {
    bigData = new Array(2000).toString()
    for (var n = 0; n < 1000; n++) {
        master.getDB(testDB).baz.insert({ n: n, data: bigData });
    }
    callGetLastError(3, 60000, testDB);

    print("replset2.js **** V1 ")

    var verifyReplication = function (nodeName, collection) {
        data = collection.findOne({ n: 1 });
        assert(data['n'] == 1, "replset2.js Failed to save to " + nodeName);
        data = collection.findOne({ n: 999 });
        assert(data['n'] == 999, "replset2.js Failed to save to " + nodeName);
    }

    print("replset2.js **** V2 ")

    verifyReplication("master", master.getDB(testDB).baz);
    verifyReplication("slave 0", slaves[0].getDB(testDB).baz);
    verifyReplication("slave 1", slaves[1].getDB(testDB).baz);

    assert(failed == false, "replset2.js Replication with getLastError failed. See errors.");
    }
    catch(e) {
      print("ERROR: " + e);
      print("Master oplog findOne:");
      printjson(master.getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
      print("Slave 0 oplog findOne:");
      printjson(slaves[0].getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
      print("Slave 1 oplog findOne:");
      printjson(slaves[1].getDB("local").oplog.rs.find().sort({"$natural": -1}).limit(1).next());
    }


    replTest.stopSet(signal);
}

doTest( 15 );

print("\nreplset2.js SUCCESS\n");
