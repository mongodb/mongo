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

    var primary = replTest.getPrimary().getDB(name);
    var secondaryConns = replTest.getSecondaries();
    var secondaries = [];
    for (var i in secondaryConns) {
        var d = secondaryConns[i].getDB(name);
        secondaries.push(d);
    }

    waitForAllMembers(primary);

    // insert a record
    assert.commandWorked(primary.foo.insert({x: 1}, {writeConcern: {w: 2}}));

    var doc = primary.foo.findOne();
    assert.eq(doc.x, 1);

    // make sure secondary has it
    var doc = secondaries[0].foo.findOne();
    assert.eq(doc.x, 1);

    // make sure delayed secondary doesn't have it
    for (var i = 0; i < 8; i++) {
        assert.eq(secondaries[1].foo.findOne(), null);
        sleep(1000);
    }

    // within 120 seconds delayed secondary should have it
    assert.soon(function() {
        var z = secondaries[1].foo.findOne();
        return z && z.x == 1;
    }, 'waiting for inserted document ' + tojson(doc) + ' on delayed secondary', 120 * 1000);

    /************* Part 2 *******************/

    // how about if we add a new server?  will it sync correctly?
    conn = replTest.add();

    config = primary.getSiblingDB("local").system.replset.findOne();
    printjson(config);
    config.version++;
    config.members.push({
        _id: 3,
        host: host + ":" + replTest.ports[replTest.ports.length - 1],
        priority: 0,
        slaveDelay: 30
    });

    primary = reconfig(replTest, config);
    primary = primary.getSiblingDB(name);

    assert.commandWorked(primary.foo.insert(
        {_id: 123, x: 'foo'}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

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
    primary = replTest.getPrimary().getDB(name);
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
    primary.foo.insert({_id: 124, "x": "foo"});
    assert(primary.foo.findOne({_id: 124}) != null);

    for (var i = 0; i < 10; i++) {
        assert.eq(conn.getDB(name).foo.findOne({_id: 124}), null);
        sleep(1000);
    }

    assert.soon(function() {
        return conn.getDB(name).foo.findOne({_id: 124}) != null;
    }, "findOne should complete within default timeout");

    replTest.stopSet();
};

doTest(15);
