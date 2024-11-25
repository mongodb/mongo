/**
 * Tests that load-balanced connections are reported correctly in server status metrics.
 * @tags: [
 *   # TODO (SERVER-85629): Re-enable this test once redness is resolved in multiversion suites.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 *   requires_fcv_80,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

const kProxyIngressPort = allocatePort();
const kProxyEgressPort = allocatePort();
const kProxyVersion = 2;

if (_isWindows()) {
    quit();
}
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

(() => {
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

    let proxy_server = new ProxyProtocolServer(kProxyIngressPort, kProxyEgressPort, kProxyVersion);
    proxy_server.start();

    var st = new ShardingTest({
        shards: 1,
        mongos: 1,
        mongosOptions: {setParameter: {"loadBalancerPort": kProxyEgressPort}}
    });
    let admin = st.s.getDB("admin");

    var uri = `mongodb://127.0.0.1:${kProxyIngressPort}/?loadBalanced=true`;

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

    st.stop();
    proxy_server.stop();
})();
