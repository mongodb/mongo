/**
 * Tests that load-balanced connections are reported correctly in server status metrics.
 */

(() => {
    "use strict";

    load('jstests/libs/fail_point_util.js');

    const numConnections = 10;

    function createTemporaryConnection(uri, dbName, collectionName) {
        // Retry connecting until you are successful
        var pollString = "var conn = null;" +
            "assert.soon(function() {" +
            "try { conn = new Mongo(\"" + uri + "\"); return conn" +
            "} catch (x) {return false;}}, " +
            "\"Timed out waiting for temporary connection to connect\", 30000, 5000);";
        // Poll the signal collection until it is told to terminate.
        pollString += "assert.soon(function() {" +
            "return conn.getDB('" + dbName + "').getCollection('" + collectionName + "')" +
            ".findOne().stop;}, \"Parallel shell never told to terminate\", 10 * 60000);";
        return startParallelShell(pollString, null, true);
    }

    function waitForConnections(db, expected) {
        assert.soon(() => admin.serverStatus().connections.loadBalanced == expected,
                    () => "Incorrect number of load-balanced connections: expected " + expected +
                        ", but serverStatus() reports " +
                        admin.serverStatus().connections.loadBalanced,
                    5 * 60000);
    }

    var st = new ShardingTest({shards: 1, mongos: 1});
    let admin = st.s.getDB("admin");

    assert.commandWorked(
        admin.adminCommand({configureFailPoint: 'clientIsFromLoadBalancer', mode: 'alwaysOn'}));

    var uri = "mongodb://" + admin.getMongo().host + "/?loadBalanced=true";

    var testDB = 'connectionsOpenedTest';
    var signalCollection = 'keepRunning';

    admin.getSiblingDB(testDB).dropDatabase();
    admin.getSiblingDB(testDB).getCollection(signalCollection).insert({stop: false});

    var connections = [];
    for (var i = 0; i < numConnections; i++) {
        connections.push(createTemporaryConnection(uri, testDB, signalCollection));
        waitForConnections(admin, i + 1);
    }

    admin.getSiblingDB(testDB).getCollection(signalCollection).update({}, {$set: {stop: true}});
    for (var i = 0; i < numConnections; i++) {
        connections[i]();
    }
    waitForConnections(admin, 0);

    assert.commandWorked(
        admin.adminCommand({configureFailPoint: 'clientIsFromLoadBalancer', mode: 'off'}));
    st.stop();
})();
