/**
 * This test checks the upgrade path for mixed mode ssl + x509 auth
 * from disabled/keyfiles up to preferSSL/x509
 *
 * NOTE: This test is similar to upgrade_to_x509_ssl_nossl.js in the
 * sslSpecial test suite. This test uses ssl communication
 * and therefore cannot test modes that do not allow ssl.
 */

// If we are running in use-x509 passthrough mode, turn it off
// since it is not necessary for this test.
TestData.useX509 = false;
load("jstests/ssl/libs/ssl_helpers.js");

opts = {sslMode:"allowSSL", sslPEMKeyFile: SERVER_CERT,
        sslAllowInvalidCertificates: "",
        clusterAuthMode:"sendKeyFile", keyFile: KEYFILE,
        sslCAFile: CA_CERT};
var rst = new ReplSetTest({ name: 'sslSet', nodes: 3, nodeOptions : opts });
rst.startSet();
rst.initiate();

// Connect to master and do some basic operations
var rstConn1 = rst.getMaster();
rstConn1.getDB("test").a.insert({a:1, str:"TESTTESTTEST"});
rstConn1.getDB("test").a.insert({a:1, str:"WOOPWOOPWOOPWOOPWOOP"});
assert.eq(2, rstConn1.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE allowSSL,sendKeyfile -> preferSSL,sendX509 =====");
rst.upgradeSet({sslMode:"preferSSL", sslPEMKeyFile: SERVER_CERT,
                sslAllowInvalidCertificates: "",
                clusterAuthMode:"sendX509", keyFile: KEYFILE,
                sslCAFile: CA_CERT});
rst.awaitReplication();
var rstConn3 = rst.getMaster();
rstConn3.getDB("test").a.insert({a:3, str:"TESTTESTTEST"});
assert.eq(3, rstConn3.getDB("test").a.count(), "Error interacting with replSet");

// Test that a non-ssl connection can still be made
var canConnectNoSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--eval", ";");
assert.eq(0, canConnectNoSSL, "SSL Connection attempt failed when it should succeed");

print("===== UPGRADE preferSSL,sendX509 -> requireSSL,x509 =====");
rst.upgradeSet({sslMode:"requireSSL", sslPEMKeyFile: SERVER_CERT,
                sslAllowInvalidCertificates: "",
                clusterAuthMode:"x509", keyFile: KEYFILE,
                sslCAFile: CA_CERT});
rst.awaitReplication();
var rstConn4 = rst.getMaster();
rstConn4.getDB("test").a.insert({a:4, str:"TESTTESTTEST"});
assert.eq(4, rstConn4.getDB("test").a.count(), "Error interacting with replSet");
