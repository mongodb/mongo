load("jstests/replsets/rslib.js");
load('jstests/replsets/libs/election_metrics.js');

function basicReplsetTest(signal, ssl_options1, ssl_options2, ssl_name) {
    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

    // Choose a name that is unique to the options specified.
    // This is important because we are depending on a fresh replicaSetMonitor for each run;
    // each differently-named replica set gets its own monitor.
    // n0 and n1 get the same SSL config since there are 3 nodes but only 2 different configs
    let replTest = new ReplSetTest({
        name: 'testSet' + ssl_name,
        nodes: {n0: ssl_options1, n1: ssl_options1, n2: ssl_options2}
    });

    // call startSet() to start each mongod in the replica set
    replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    // Call getPrimary to return a reference to the node that's been
    // elected primary.
    let primary = replTest.getPrimary();

    // Check that both the 'called' and 'successful' fields of the 'electionTimeout' election reason
    // counter have been incremented in serverStatus.
    const primaryStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusElectionReasonCounterValue(
        primaryStatus.electionMetrics, "electionTimeout", 1);

    // Ensure the primary logs an n-op to the oplog upon transitioning to primary.
    assert.gt(primary.getDB("local").oplog.rs.count({op: 'n', o: {msg: 'new primary'}}), 0);

    // Here's how you save something to primary
    primary.getDB("foo").foo.save({a: 1000});

    // This method will check the oplogs of the primary
    // and secondaries in the set and wait until the change has replicated.
    replTest.awaitReplication();

    let cppconn = new Mongo(replTest.getURL()).getDB("foo");
    assert.eq(1000, cppconn.foo.findOne().a, "cppconn 1");

    {
        // check c++ finding other servers
        let temp = replTest.getURL();
        temp = temp.substring(0, temp.lastIndexOf(","));
        temp = new Mongo(temp).getDB("foo");
        assert.eq(1000, temp.foo.findOne().a, "cppconn 1");
    }

    // Here's how to stop the primary node
    let primaryId = replTest.getNodeId(primary);
    replTest.stop(primaryId);

    // Now let's see who the new primary is:
    let newPrimary = replTest.getPrimary();

    // Is the new primary the same as the old primary?
    let newPrimaryId = replTest.getNodeId(newPrimary);

    assert(primaryId != newPrimaryId, "Old primary shouldn't be equal to new primary.");

    reconnect(cppconn);
    assert.eq(1000, cppconn.foo.findOne().a, "cppconn 2");

    // Now let's write some documents to the new primary
    let bulk = newPrimary.getDB("bar").bar.initializeUnorderedBulkOp();
    for (let i = 0; i < 1000; i++) {
        bulk.insert({a: i});
    }
    bulk.execute();

    // Here's how to restart the old primary node:
    let secondary = replTest.restart(primaryId);

    // Now, let's make sure that the old primary comes up as a secondary
    assert.soon(function() {
        let res = secondary.getDB("admin").runCommand({hello: 1});
        printjson(res);
        return res['ok'] == 1 && res['isWritablePrimary'] == false;
    });

    // And we need to make sure that the replset comes back up
    assert.soon(function() {
        let res = newPrimary.getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(res);
        return res.myState == 1;
    });

    // And that both secondary nodes have all the updates
    newPrimary = replTest.getPrimary();
    assert.eq(1000, newPrimary.getDB("bar").runCommand({count: "bar"}).n, "assumption 2");
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    let secondaries = replTest.getSecondaries();
    assert(secondaries.length == 2, "Expected 2 secondaries but length was " + secondaries.length);
    secondaries.forEach(function(secondary) {
        secondary.setSecondaryOk();
        let count = secondary.getDB("bar").runCommand({count: "bar"});
        printjson(count);
        assert.eq(1000, count.n, "secondary count wrong: " + secondary);
    });

    // last error
    primary = replTest.getPrimary();
    secondaries = replTest.getSecondaries();

    let db = primary.getDB("foo");
    let t = db.foo;

    let ts = secondaries.map(function(z) {
        z.setSecondaryOk();
        return z.getDB("foo").foo;
    });

    t.save({a: 1000});
    t.createIndex({a: 1});
    replTest.awaitReplication();

    ts.forEach(function(z) {
        assert.eq(2, z.getIndexKeys().length, "A " + z.getMongo());
    });

    // Shut down the set and finish the test.
    replTest.stopSet(signal);
}
