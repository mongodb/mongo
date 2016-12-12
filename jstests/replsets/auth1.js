// check replica set authentication
//
// This test requires users to persist across a restart.
// @tags: [requires_persistence]

load("jstests/replsets/rslib.js");

(function() {
    var name = "rs_auth1";
    var port = allocatePorts(5);
    var path = "jstests/libs/";

    // These keyFiles have their permissions set to 600 later in the test.
    var key1_600 = path + "key1";
    var key2_600 = path + "key2";

    // This keyFile has its permissions set to 644 later in the test.
    var key1_644 = path + "key1_644";

    print("try starting mongod with auth");
    var m = MongoRunner.runMongod(
        {auth: "", port: port[4], dbpath: MongoRunner.dataDir + "/wrong-auth"});

    assert.eq(m.getDB("local").auth("__system", ""), 0);

    MongoRunner.stopMongod(m);

    print("reset permissions");
    run("chmod", "644", key1_644);

    print("try starting mongod");
    m = runMongoProgram("mongod",
                        "--keyFile",
                        key1_644,
                        "--port",
                        port[0],
                        "--dbpath",
                        MongoRunner.dataPath + name);

    print("should fail with wrong permissions");
    assert.eq(
        m, _isWindows() ? 100 : 1, "mongod should exit w/ 1 (EXIT_FAILURE): permissions too open");
    MongoRunner.stopMongod(port[0]);

    // Pre-populate the data directory for the first replica set node, to be started later, with
    // a user's credentials.
    print("add a user to server0: foo");
    m = MongoRunner.runMongod({dbpath: MongoRunner.dataPath + name + "-0"});
    m.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});
    m.getDB("test").createUser({user: "bar", pwd: "baz", roles: jsTest.basicUserRoles});
    print("make sure user is written before shutting down");
    MongoRunner.stopMongod(m);

    print("start up rs");
    var rs = new ReplSetTest({"name": name, "nodes": 3});

    // The first node is started with the pre-populated data directory.
    print("start 0 with keyFile");
    m = rs.start(0, {"keyFile": key1_600, noCleanData: true});
    print("start 1 with keyFile");
    rs.start(1, {"keyFile": key1_600});
    print("start 2 with keyFile");
    rs.start(2, {"keyFile": key1_600});

    var result = m.getDB("admin").auth("foo", "bar");
    assert.eq(result, 1, "login failed");
    print("Initializing replSet with config: " + tojson(rs.getReplSetConfig()));
    result = m.getDB("admin").runCommand({replSetInitiate: rs.getReplSetConfig()});
    assert.eq(result.ok, 1, "couldn't initiate: " + tojson(result));
    m.getDB('admin')
        .logout();  // In case this node doesn't become primary, make sure its not auth'd

    var master = rs.getPrimary();
    rs.awaitSecondaryNodes();
    var mId = rs.getNodeId(master);
    var slave = rs.liveNodes.slaves[0];
    assert.eq(1, master.getDB("admin").auth("foo", "bar"));
    assert.writeOK(
        master.getDB("test").foo.insert({x: 1}, {writeConcern: {w: 3, wtimeout: 60000}}));

    print("try some legal and illegal reads");
    var r = master.getDB("test").foo.findOne();
    assert.eq(r.x, 1);

    slave.setSlaveOk();

    function doQueryOn(p) {
        var error = assert
                        .throws(
                            function() {
                                r = p.getDB("test").foo.findOne();
                            },
                            [],
                            "find did not throw, returned: " + tojson(r))
                        .toString();
        printjson(error);
        assert.gt(error.indexOf("not authorized"), -1, "error was non-auth");
    }

    doQueryOn(slave);
    master.adminCommand({logout: 1});

    print("unauthorized:");
    printjson(master.adminCommand({replSetGetStatus: 1}));

    doQueryOn(master);

    result = slave.getDB("test").auth("bar", "baz");
    assert.eq(result, 1);

    r = slave.getDB("test").foo.findOne();
    assert.eq(r.x, 1);

    print("add some data");
    master.getDB("test").auth("bar", "baz");
    var bulk = master.getDB("test").foo.initializeUnorderedBulkOp();
    for (var i = 0; i < 1000; i++) {
        bulk.insert({x: i, foo: "bar"});
    }
    assert.writeOK(bulk.execute({w: 3, wtimeout: 60000}));

    print("fail over");
    rs.stop(mId);

    master = rs.getPrimary();

    print("add some more data 1");
    master.getDB("test").auth("bar", "baz");
    bulk = master.getDB("test").foo.initializeUnorderedBulkOp();
    for (var i = 0; i < 1000; i++) {
        bulk.insert({x: i, foo: "bar"});
    }
    assert.writeOK(bulk.execute({w: 2}));

    print("resync");
    rs.restart(mId, {"keyFile": key1_600});
    master = rs.getPrimary();

    print("add some more data 2");
    bulk = master.getDB("test").foo.initializeUnorderedBulkOp();
    for (var i = 0; i < 1000; i++) {
        bulk.insert({x: i, foo: "bar"});
    }
    bulk.execute({w: 3, wtimeout: 60000});

    print("add member with wrong key");
    var conn = MongoRunner.runMongod({
        dbpath: MongoRunner.dataPath + name + "-3",
        port: port[3],
        replSet: "rs_auth1",
        oplogSize: 2,
        keyFile: key2_600
    });

    master.getDB("admin").auth("foo", "bar");
    var config = master.getDB("local").system.replset.findOne();
    config.members.push({_id: 3, host: rs.host + ":" + port[3]});
    config.version++;
    try {
        master.adminCommand({replSetReconfig: config});
    } catch (e) {
        print("error: " + e);
    }
    master = rs.getPrimary();
    master.getDB("admin").auth("foo", "bar");

    print("shouldn't ever sync");
    for (var i = 0; i < 10; i++) {
        print("iteration: " + i);
        var results = master.adminCommand({replSetGetStatus: 1});
        printjson(results);
        assert(results.members[3].state != 2);
        sleep(1000);
    }

    print("stop member");
    MongoRunner.stopMongod(conn);

    print("start back up with correct key");
    var conn = MongoRunner.runMongod({
        dbpath: MongoRunner.dataPath + name + "-3",
        port: port[3],
        replSet: "rs_auth1",
        oplogSize: 2,
        keyFile: key1_600
    });

    wait(function() {
        try {
            var results = master.adminCommand({replSetGetStatus: 1});
            printjson(results);
            return results.members[3].state == 2;
        } catch (e) {
            print(e);
        }
        return false;
    });

    print("make sure it has the config, too");
    assert.soon(function() {
        for (var i in rs.nodes) {
            rs.nodes[i].setSlaveOk();
            rs.nodes[i].getDB("admin").auth("foo", "bar");
            config = rs.nodes[i].getDB("local").system.replset.findOne();
            if (config.version != 2) {
                return false;
            }
        }
        return true;
    });
})();
