var ssl_options1;
var ssl_options2;
var ssl_name;
load("jstests/replsets/rslib.js");
var doTest = function(signal) {

    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

    // Choose a name that is unique to the options specified.
    // This is important because we are depending on a fresh replicaSetMonitor for each run;
    // each differently-named replica set gets its own monitor.
    // n0 and n1 get the same SSL config since there are 3 nodes but only 2 different configs
    var replTest = new ReplSetTest({
        name: 'testSet' + ssl_name,
        nodes: {n0: ssl_options1, n1: ssl_options1, n2: ssl_options2}
    });

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    var nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    // Call getPrimary to return a reference to the node that's been
    // elected master.
    var master = replTest.getPrimary();

    var isPV1 = (replTest.getReplSetConfigFromNode().protocolVersion == 1);
    if (isPV1) {
        // Ensure the primary logs an n-op to the oplog upon transitioning to primary.
        assert.gt(master.getDB("local").oplog.rs.count({op: 'n', o: {msg: 'new primary'}}), 0);
    }
    // Calling getPrimary also makes available the liveNodes structure,
    // which looks like this:
    // liveNodes = {master: masterNode,
    //              slaves: [slave1, slave2]
    //             }
    printjson(replTest.liveNodes);

    // Here's how you save something to master
    master.getDB("foo").foo.save({a: 1000});

    // This method will check the oplogs of the master
    // and slaves in the set and wait until the change has replicated.
    replTest.awaitReplication();

    var cppconn = new Mongo(replTest.getURL()).getDB("foo");
    assert.eq(1000, cppconn.foo.findOne().a, "cppconn 1");

    {
        // check c++ finding other servers
        var temp = replTest.getURL();
        temp = temp.substring(0, temp.lastIndexOf(","));
        temp = new Mongo(temp).getDB("foo");
        assert.eq(1000, temp.foo.findOne().a, "cppconn 1");
    }

    // Here's how to stop the master node
    var master_id = replTest.getNodeId(master);
    replTest.stop(master_id);

    // Now let's see who the new master is:
    var new_master = replTest.getPrimary();

    // Is the new master the same as the old master?
    var new_master_id = replTest.getNodeId(new_master);

    assert(master_id != new_master_id, "Old master shouldn't be equal to new master.");

    reconnect(cppconn);
    assert.eq(1000, cppconn.foo.findOne().a, "cppconn 2");

    // Now let's write some documents to the new master
    var bulk = new_master.getDB("bar").bar.initializeUnorderedBulkOp();
    for (var i = 0; i < 1000; i++) {
        bulk.insert({a: i});
    }
    bulk.execute();

    // Here's how to restart the old master node:
    var slave = replTest.restart(master_id);

    // Now, let's make sure that the old master comes up as a slave
    assert.soon(function() {
        var res = slave.getDB("admin").runCommand({ismaster: 1});
        printjson(res);
        return res['ok'] == 1 && res['ismaster'] == false;
    });

    // And we need to make sure that the replset comes back up
    assert.soon(function() {
        var res = new_master.getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(res);
        return res.myState == 1;
    });

    // And that both slave nodes have all the updates
    new_master = replTest.getPrimary();
    assert.eq(1000, new_master.getDB("bar").runCommand({count: "bar"}).n, "assumption 2");
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    var slaves = replTest.liveNodes.slaves;
    assert(slaves.length == 2, "Expected 2 slaves but length was " + slaves.length);
    slaves.forEach(function(slave) {
        slave.setSlaveOk();
        var count = slave.getDB("bar").runCommand({count: "bar"});
        printjson(count);
        assert.eq(1000, count.n, "slave count wrong: " + slave);
    });

    // last error
    master = replTest.getPrimary();
    slaves = replTest.liveNodes.slaves;
    printjson(replTest.liveNodes);

    var db = master.getDB("foo");
    var t = db.foo;

    var ts = slaves.map(function(z) {
        z.setSlaveOk();
        return z.getDB("foo").foo;
    });

    t.save({a: 1000});
    t.ensureIndex({a: 1});

    var result = db.runCommand({getLastError: 1, w: 3, wtimeout: 30000});
    printjson(result);
    var lastOp = result.lastOp;
    var lastOplogOp = master.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();
    if (replTest.getReplSetConfigFromNode().protocolVersion != 1) {
        assert.eq(lastOplogOp['ts'], lastOp);
    } else {
        assert.eq(lastOplogOp['ts'], lastOp['ts']);
        assert.eq(lastOplogOp['t'], lastOp['t']);
    }

    ts.forEach(function(z) {
        assert.eq(2, z.getIndexKeys().length, "A " + z.getMongo());
    });

    t.reIndex();

    db.getLastError(3, 30000);
    ts.forEach(function(z) {
        assert.eq(2, z.getIndexKeys().length, "A " + z.getMongo());
    });

    // Shut down the set and finish the test.
    replTest.stopSet(signal);
};

doTest(15);
print("replset1.js SUCCESS");
