// rs test getlasterrordefaults

doTest = function (signal) {

    // Test getLastError defaults
    var replTest = new ReplSetTest({ name: 'testSet', nodes: 3 });

    var nodes = replTest.startSet();

    // Initiate set with default for getLastError
    var config = replTest.getReplSetConfig();
    config.settings = {};
    config.settings.getLastErrorDefaults = { 'w': 3, 'wtimeout': 20000 };
    config.settings.heartbeatTimeoutSecs = 15;

    replTest.initiate(config);

    //
    var master = replTest.getMaster();
    replTest.awaitSecondaryNodes();
    var testDB = "foo";

    // Initial replication
    master.getDB("barDB").bar.save({ a: 1 });
    replTest.awaitReplication();

    // These writes should be replicated immediately
    var docNum = 5000;
    for (var n = 0; n < docNum; n++) {
        master.getDB(testDB).foo.insert({ n: n });
    }

    // should use the configured last error defaults from above, that's what we're testing.
    // 
    // If you want to test failure, just add values for w and wtimeout (e.g. w=1)
    // to the following command. This will override the default set above and
    // prevent replication from happening in time for the count tests below.
    //
    var result = master.getDB("admin").runCommand({ getlasterror: 1 });
    print("replset5.js getlasterror result:");
    printjson(result);

    if (result.err == "timeout") {
        print("\WARNING getLastError timed out and should not have.\nThis machine seems extremely slow. Stopping test without failing it\n")
        replTest.stopSet(signal);
        print("\WARNING getLastError timed out and should not have.\nThis machine seems extremely slow. Stopping test without failing it\n")
        return;
    }

    var slaves = replTest.liveNodes.slaves;
    slaves[0].setSlaveOk();
    slaves[1].setSlaveOk();

    print("replset5.js Testing slave counts");

    var slave0count = slaves[0].getDB(testDB).foo.count();
    assert(slave0count == docNum, "Slave 0 has " + slave0count + " of " + docNum + " documents!");

    var slave1count = slaves[1].getDB(testDB).foo.count();
    assert(slave1count == docNum, "Slave 1 has " + slave1count + " of " + docNum + " documents!");

    var master1count = master.getDB(testDB).foo.count();
    assert(master1count == docNum, "Master has " + master1count + " of " + docNum + " documents!");

    print("replset5.js reconfigure with hidden=1");
    config = master.getDB("local").system.replset.findOne();

    assert.eq(15, config.settings.heartbeatTimeoutSecs);

    config.version++;
    config.members[2].priority = 0;
    config.members[2].hidden = 1;

    try {
        master.adminCommand({ replSetReconfig: config });
    }
    catch (e) {
        print(e);
    }

    config = master.getDB("local").system.replset.findOne();
    printjson(config);
    assert.eq(config.members[2].hidden, true);

    replTest.stopSet(signal);
}

doTest( 15 );
print("replset5.js success");
