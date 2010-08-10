doTest = function (signal) {

    // Test getLastError defaults
    var replTest = new ReplSetTest({ name: 'testSet', nodes: 3 });

    var nodes = replTest.startSet();

    // Initiate set with default for getLastError
    var config = replTest.getReplSetConfig();
    config.settings = {};
    config.settings.getLastErrorDefaults = { 'w': 3, 'wtimeout': 20000 };

    replTest.initiate(config);

    //
    var master = replTest.getMaster();
    replTest.awaitSecondaryNodes();
    var testDB = "foo";

    // Initial replication
    master.getDB("barDB").bar.save({ a: 1 });
    replTest.awaitReplication();


    // These writes should be replicated immediately
    master.getDB(testDB).foo.insert({ n: 1 });
    master.getDB(testDB).foo.insert({ n: 2 });
    master.getDB(testDB).foo.insert({ n: 3 });

    // *** NOTE ***: The default doesn't seem to be propogating.
    // When I run getlasterror with no defaults, the slaves don't have the data:
    // These getlasterror commands can be run individually to verify this.
    //master.getDB("admin").runCommand({ getlasterror: 1, w: 3, wtimeout: 20000 });
    master.getDB("admin").runCommand({getlasterror: 1});

    var slaves = replTest.liveNodes.slaves;
    slaves[0].setSlaveOk();
    slaves[1].setSlaveOk();

    print("Testing slave counts");

    // These should all have 3 documents, but they don't always.
    var master1count = master.getDB(testDB).foo.count();
    assert( master1count == 3, "Master has " + master1count + " of 3 documents!");

    var slave0count = slaves[0].getDB(testDB).foo.count();
    assert( slave0count == 3, "Slave 0 has " + slave0count + " of 3 documents!");

    var slave1count = slaves[1].getDB(testDB).foo.count();
    assert( slave1count == 3, "Slave 1 has " + slave1count + " of 3 documents!");

    print("Testing slave 0");

    var s0 = slaves[0].getDB(testDB).foo.find();
    assert(s0.next()['n']);
    assert(s0.next()['n']);
    assert(s0.next()['n']);

    print("Testing slave 1");

    var s1 = slaves[1].getDB(testDB).foo.find();
    assert(s1.next()['n']);
    assert(s1.next()['n']);
    assert(s1.next()['n']);

    // End test
    replTest.stopSet(signal);
}

//doTest( 15 );
