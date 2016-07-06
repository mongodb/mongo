// rs test getlasterrordefaults
load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    // Test write concern defaults
    var replTest = new ReplSetTest({name: 'testSet', nodes: 3});

    var nodes = replTest.startSet();

    // Initiate set with default for write concern
    var config = replTest.getReplSetConfig();
    config.settings = {};
    config.settings.getLastErrorDefaults = {'w': 3, 'wtimeout': 20000};
    config.settings.heartbeatTimeoutSecs = 15;
    // Prevent node 2 from becoming primary, as we will attempt to set it to hidden later.
    config.members[2].priority = 0;

    replTest.initiate(config);

    //
    var master = replTest.getPrimary();
    replTest.awaitSecondaryNodes();
    var testDB = "foo";

    // Initial replication
    master.getDB("barDB").bar.save({a: 1});
    replTest.awaitReplication();

    // These writes should be replicated immediately
    var docNum = 5000;
    var bulk = master.getDB(testDB).foo.initializeUnorderedBulkOp();
    for (var n = 0; n < docNum; n++) {
        bulk.insert({n: n});
    }

    // should use the configured last error defaults from above, that's what we're testing.
    //
    // If you want to test failure, just add values for w and wtimeout (e.g. w=1)
    // to the following command. This will override the default set above and
    // prevent replication from happening in time for the count tests below.
    //
    var result = bulk.execute();
    var wcError = result.getWriteConcernError();

    if (wcError != null) {
        print("\WARNING getLastError timed out and should not have: " + result.toString());
        print("This machine seems extremely slow. Stopping test without failing it\n");
        replTest.stopSet();
        return;
    }

    var slaves = replTest.liveNodes.slaves;
    slaves[0].setSlaveOk();
    slaves[1].setSlaveOk();

    var slave0count = slaves[0].getDB(testDB).foo.find().itcount();
    assert(slave0count == docNum, "Slave 0 has " + slave0count + " of " + docNum + " documents!");

    var slave1count = slaves[1].getDB(testDB).foo.find().itcount();
    assert(slave1count == docNum, "Slave 1 has " + slave1count + " of " + docNum + " documents!");

    var master1count = master.getDB(testDB).foo.find().itcount();
    assert(master1count == docNum, "Master has " + master1count + " of " + docNum + " documents!");

    print("replset5.js reconfigure with hidden=1");
    config = master.getDB("local").system.replset.findOne();

    assert.eq(15, config.settings.heartbeatTimeoutSecs);

    config.version++;
    config.members[2].hidden = 1;

    master = reconfig(replTest, config);

    config = master.getSisterDB("local").system.replset.findOne();
    assert.eq(config.members[2].hidden, true);

    replTest.stopSet();
}());
