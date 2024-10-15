/**
 * This test checks the upgrade path for mixed mode ssl + x509 auth
 * from disabled/keyfiles up to preferTLS/x509
 *
 * NOTE: This test is similar to upgrade_to_x509_ssl_nossl.js in the
 * sslSpecial test suite. This test uses ssl communication
 * and therefore cannot test modes that do not allow ssl.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {CA_CERT, KEYFILE, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

function authAllNodes() {
    for (var n = 0; n < rst.nodes.length; n++) {
        var status = rst.nodes[n].getDB("admin").auth("root", "pwd");
        assert.eq(status, 1);
    }
}

let opts = {
    tlsMode: "allowTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsAllowInvalidCertificates: "",
    clusterAuthMode: "sendKeyFile",
    keyFile: KEYFILE,
    tlsCAFile: CA_CERT
};
var NUM_NODES = 3;
var rst = new ReplSetTest({
    name: 'tlsSet',
    nodes: NUM_NODES,
    nodeOptions: opts,
    waitForKeys: false,
});
rst.startSet();

rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// Connect to master and do some basic operations
var rstConn1 = rst.getPrimary();
print("Performing basic operations on master.");
rstConn1.getDB("admin").createUser({user: "root", pwd: "pwd", roles: ["root"]}, {w: NUM_NODES});
rstConn1.getDB("admin").auth("root", "pwd");
rstConn1.getDB("test").a.insert({a: 1, str: "TESTTESTTEST"});
rstConn1.getDB("test").a.insert({a: 1, str: "WOOPWOOPWOOPWOOPWOOP"});
assert.eq(2, rstConn1.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE allowTLS,sendKeyfile -> preferTLS,sendX509 =====");
authAllNodes();
rst.awaitReplication();
rst.upgradeSet({
    tlsMode: "preferTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsAllowInvalidCertificates: "",
    clusterAuthMode: "sendX509",
    keyFile: KEYFILE,
    tlsCAFile: CA_CERT
},
               "root",
               "pwd");
// The upgradeSet call restarts the nodes so we need to reauthenticate.
authAllNodes();
var rstConn3 = rst.getPrimary();
rstConn3.getDB("test").a.insert({a: 3, str: "TESTTESTTEST"});
assert.eq(3, rstConn3.getDB("test").a.count(), "Error interacting with replSet");
rst.awaitReplication();
// Test that a non-ssl connection can still be made
var canConnectNoSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--eval", ";");
assert.eq(0, canConnectNoSSL, "SSL Connection attempt failed when it should succeed");

print("===== UPGRADE preferTLS,sendX509 -> requireTLS,x509 =====");
rst.upgradeSet({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsAllowInvalidCertificates: "",
    clusterAuthMode: "x509",
    keyFile: KEYFILE,
    tlsCAFile: CA_CERT
},
               "root",
               "pwd");
authAllNodes();
var rstConn4 = rst.getPrimary();
rstConn4.getDB("test").a.insert({a: 4, str: "TESTTESTTEST"});
assert.eq(4, rstConn4.getDB("test").a.count(), "Error interacting with replSet");
rst.stopSet();
