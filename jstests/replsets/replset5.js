// rs test getlasterrordefaults

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
    var docNum = 5000;
    for(var n=0; n<docNum; n++) {
      master.getDB(testDB).foo.insert({ n: n });
    }

    // *** NOTE ***: The default doesn't seem to be propogating.
    // When I run getlasterror with no defaults, the slaves don't have the data:
    // These getlasterror commands can be run individually to verify this.
    //master.getDB("admin").runCommand({ getlasterror: 1, w: 1, wtimeout: 20000 });
    master.getDB("admin").runCommand({getlasterror: 1});

    var slaves = replTest.liveNodes.slaves;
    slaves[0].setSlaveOk();
    slaves[1].setSlaveOk();

    print("Testing slave counts");

    var slave0count = slaves[0].getDB(testDB).foo.count();
    assert( slave0count == docNum, "Slave 0 has " + slave0count + " of " + docNum + " documents!");

    var slave1count = slaves[1].getDB(testDB).foo.count();
    assert( slave1count == docNum, "Slave 1 has " + slave1count + " of " + docNum + " documents!");

    var master1count = master.getDB(testDB).foo.count();
    assert( master1count == docNum, "Master has " + master1count + " of " + docNum + " documents!");

    replTest.stopSet(signal);
}

doTest( 15 );
print("replset5.js success");
