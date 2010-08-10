doTest = function (signal) {

    // Test getLastError defaults
    var replTest = new ReplSetTest({ name: 'testSet', nodes: 3 });

    var nodes = replTest.startSet();

    // Initiate set with default for getLastError
    var config = replTest.getReplSetConfig();
    config.settings = {};
    config.settings.getLastErrorDefaults = { 'w': 3, 'wtimeout': 10000 };

    replTest.initiate(config);

    //
    var master = replTest.getMaster();
    replTest.awaitSecondaryNodes();
    var testDB = "foo";

    // Initial replication
    master.getDB(testDB).bar.save({ a: 1 });
    replTest.awaitReplication();

    var slaves = replTest.liveNodes.slaves;

    // These writes should be replicated immediately
    master.getDB(testDB).foo.insert({ n: 1 });
    master.getDB(testDB).foo.insert({ n: 2 });
    master.getDB(testDB).foo.insert({ n: 3 });

    // *** NOTE ***: The slaves have the data when I run this:
    master.getDB("admin").runCommand({ getlasterror: 1, w: 3, wtimeout: 5000 });

    // But when I run the test with no defaults, they don't:
    // master.getDB("admin").runCommand({getlasterror: 1});

    slaves[0].setSlaveOk();
    slaves[1].setSlaveOk();

    print("Testing slave I");

    var s0 = slaves[0].getDB(testDB).foo.find();
    assert(s0.next()['n']);
    assert(s0.next()['n']);
    assert(s0.next()['n']);

    print("Testing slave II");

    var s1 = slaves[1].getDB(testDB).foo.find();
    assert(s1.next()['n']);
    assert(s1.next()['n']);
    assert(s1.next()['n']);

    // Let's re-initialize the replica set with a new getlasterror
    var config = replTest.getReplSetConfig();
    var c = master.getDB("local")['system.replset'].findOne();
    printjson(c);
    config.settings = {};
    config.settings.getLastErrorDefaults = { 'w': 5, 'wtimeout': 1000000 };
    config.version = c.version + 1;

    print("Reinitiating");
    replTest.initiate(config, 'replSetReconfig');

    var master = replTest.getMaster();

    master.getDB(testDB).baz.insert({ n: 3 });

    // *** NOTE ***: This should never return given the timeout set above, but it returns right away.
    print("NOTE");
    master.getDB(testDB).runCommand({ getlasterror: 1, w: 5, wtimeout: 5000000 });
    print("NOTE2");

    // End test
    replTest.stopSet(signal);
}

//doTest( 15 );
