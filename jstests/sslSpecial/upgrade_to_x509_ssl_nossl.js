/**
 * This test checks the upgrade path for mixed mode ssl + x509 auth
 * from disabled/keyfiles up to preferSSL/x509
 *
 * NOTE: This test is similar to upgrade_to_x509_ssl.js in the
 * ssl test suite. This test cannot use ssl communication
 * and therefore cannot test modes that only allow ssl.
 */

load("jstests/ssl/libs/ssl_helpers.js");

function authAllNodes() {
    for (var n = 0; n < rst.nodes.length; n++) {
        var status = rst.nodes[n].getDB("admin").auth("root", "pwd");
        assert.eq(status, 1);
    }
};

opts = {sslMode:"disabled", clusterAuthMode:"keyFile", keyFile: KEYFILE}
var NUM_NODES = 3;
var rst = new ReplSetTest({ name: 'sslSet', nodes: NUM_NODES, nodeOptions : opts });
rst.startSet();
rst.initiate();

// Connect to master and do some basic operations
var rstConn1 = rst.getMaster();
rstConn1.getDB("admin").createUser({user: "root", pwd: "pwd", roles: ["root"]}, {w: NUM_NODES});
rstConn1.getDB("admin").auth("root", "pwd");
rstConn1.getDB("test").a.insert({a:1, str:"TESTTESTTEST"});
assert.eq(1, rstConn1.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE disabled,keyFile -> allowSSL,sendKeyfile =====");
authAllNodes();
rst.upgradeSet({sslMode:"allowSSL", sslPEMKeyFile: SERVER_CERT,
                sslAllowInvalidCertificates:"",
                clusterAuthMode:"sendKeyFile", keyFile: KEYFILE,
                sslCAFile: CA_CERT}, "root", "pwd");
authAllNodes();
rst.awaitReplication();

var rstConn2 = rst.getMaster();
rstConn2.getDB("test").a.insert({a:2, str:"CHECKCHECKCHECK"});
assert.eq(2, rstConn2.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE allowSSL,sendKeyfile -> preferSSL,sendX509 =====");
rst.upgradeSet({sslMode:"preferSSL", sslPEMKeyFile: SERVER_CERT,
                sslAllowInvalidCertificates:"",
                clusterAuthMode:"sendX509", keyFile: KEYFILE,
                sslCAFile: CA_CERT}, "root", "pwd");
authAllNodes();
rst.awaitReplication();

var rstConn3 = rst.getMaster();
rstConn3.getDB("test").a.insert({a:3, str:"PEASandCARROTS"});
assert.eq(3, rstConn3.getDB("test").a.count(), "Error interacting with replSet");

var canConnectSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--ssl",
                                    "--sslAllowInvalidCertificates",
                                    "--sslPEMKeyFile",  CLIENT_CERT, "--eval", ";");
assert.eq(0, canConnectSSL, "SSL Connection attempt failed when it should succeed");

print("===== UPGRADE preferSSL,sendX509 -> preferSSL,x509 =====");
//we cannot upgrade past preferSSL here because it will break the test client
rst.upgradeSet({sslMode:"preferSSL", sslPEMKeyFile: SERVER_CERT,
                sslAllowInvalidCertificates:"",
                clusterAuthMode:"x509", keyFile: KEYFILE,
                sslCAFile: CA_CERT}, "root", "pwd");
authAllNodes();
rst.awaitReplication();
var rstConn4 = rst.getMaster();
rstConn4.getDB("test").a.insert({a:4, str:"BEEP BOOP"});
rst.awaitReplication();
assert.eq(4, rstConn4.getDB("test").a.count(), "Error interacting with replSet");

// Test that an ssl connection can still be made
var canConnectSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--ssl",
                                    "--sslAllowInvalidCertificates",
                                    "--sslPEMKeyFile",  CLIENT_CERT, "--eval", ";");
assert.eq(0, canConnectSSL, "SSL Connection attempt failed when it should succeed");
