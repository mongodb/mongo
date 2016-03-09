load("jstests/replsets/rslib.js");

doTest = function(signal) {

    var name = "slaveDelay";
    var host = getHostName();

    var replTest = new ReplSetTest({name: name, nodes: 3});

    var nodes = replTest.startSet();

    /* set slaveDelay to 30 seconds */
    var config = replTest.getReplSetConfig();
    config.members[2].priority = 0;
    config.members[2].slaveDelay = 30;

    replTest.initiate(config);

    var master = replTest.getPrimary().getDB(name);
    var slaveConns = replTest.liveNodes.slaves;
    var slaves = [];
    for (var i in slaveConns) {
        var d = slaveConns[i].getDB(name);
        slaves.push(d);
    }

    waitForAllMembers(master);

    // insert a record
    assert.writeOK(master.foo.insert({x: 1}, {writeConcern: {w: 2}}));

    var doc = master.foo.findOne();
    assert.eq(doc.x, 1);

    // make sure slave has it
    var doc = slaves[0].foo.findOne();
    assert.eq(doc.x, 1);

    // make sure delayed slave doesn't have it
    for (var i = 0; i < 8; i++) {
        assert.eq(slaves[1].foo.findOne(), null);
        sleep(1000);
    }

    // within 30 seconds delayed slave should have it
    assert.soon(function() {
        var z = slaves[1].foo.findOne();
        return z && z.x == 1;
    });

    /************* Part 2 *******************/

    // how about if we add a new server?  will it sync correctly?
    conn = replTest.add();

    config = master.getSisterDB("local").system.replset.findOne();
    printjson(config);
    config.version++;
    config.members.push({
        _id: 3,
        host: host + ":" + replTest.ports[replTest.ports.length - 1],
        priority: 0,
        slaveDelay: 30
    });

    master = reconfig(replTest, config);
    master = master.getSisterDB(name);

    // wait for the node to catch up
    replTest.awaitReplication(90 * 1000);

    assert.writeOK(master.foo.insert({_id: 123, x: 'foo'}, {writeConcern: {w: 2}}));

    for (var i = 0; i < 8; i++) {
        assert.eq(conn.getDB(name).foo.findOne({_id: 123}), null);
        sleep(1000);
    }

    assert.soon(function() {
        var z = conn.getDB(name).foo.findOne({_id: 123});
        return z != null && z.x == "foo";
    });

    /************* Part 3 ******************/

    print("reconfigure slavedelay");

    config.version++;
    config.members[3].slaveDelay = 15;

    reconfig(replTest, config);
    master = replTest.getPrimary().getDB(name);
    assert.soon(function() {
        return conn.getDB("local").system.replset.findOne().version == config.version;
    });

    // wait for node to become secondary
    assert.soon(function() {
        var result = conn.getDB("admin").isMaster();
        printjson(result);
        return result.secondary;
    });

    print("testing insert");
    master.foo.insert({_id: 124, "x": "foo"});
    assert(master.foo.findOne({_id: 124}) != null);

    for (var i = 0; i < 10; i++) {
        assert.eq(conn.getDB(name).foo.findOne({_id: 124}), null);
        sleep(1000);
    }

    // the node should have the document in 15 seconds (20 for some safety against races)
    assert.soon(function() {
        return conn.getDB(name).foo.findOne({_id: 124}) != null;
    }, 10 * 1000);

    replTest.stopSet();
};

doTest(15);
