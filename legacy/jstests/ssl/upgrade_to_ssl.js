/**
 * This test checks the upgrade path for mixed mode ssl
 * from allowSSL up to requireSSL
 *
 * NOTE: This test is similar to upgrade_to_ssl_nossl.js in the
 * sslSpecial test suite. This test uses ssl communication
 * and therefore cannot test modes that do not allow ssl.
 */

// If we are running in use-x509 passthrough mode, turn it off
// since it is not necessary for this test.
TestData.useX509 = false;
load("jstests/ssl/libs/ssl_helpers.js");

// "sslAllowInvalidCertificates" is enabled to avoid hostname conflicts with our testing certs
opts = {sslMode:"allowSSL", sslPEMKeyFile: SERVER_CERT, sslAllowInvalidCertificates: ""};
var rst = new ReplSetTest({ name: 'sslSet', nodes: 3, nodeOptions : opts });
rst.startSet();
rst.initiate();

var rstConn1 = rst.getMaster();
rstConn1.getDB("test").a.insert({a:1, str:"TESTTESTTEST"});
assert.eq(1, rstConn1.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE allowSSL -> preferSSL =====");
rst.upgradeSet({sslMode:"preferSSL", sslPEMKeyFile: SERVER_CERT, sslAllowInvalidCertificates: ""});
var rstConn2 = rst.getMaster();
rstConn2.getDB("test").a.insert({a:2, str:"CHECKCHECK"});
assert.eq(2, rstConn2.getDB("test").a.count(), "Error interacting with replSet");

// Check that non-ssl connections can still be made
var canConnectNoSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--eval", ";");
assert.eq(0, canConnectNoSSL, "non-SSL Connection attempt failed when it should succeed");

print("===== UPGRADE preferSSL -> requireSSL =====");
rst.upgradeSet({sslMode:"requireSSL", sslPEMKeyFile: SERVER_CERT, sslAllowInvalidCertificates: ""});
var rstConn3 = rst.getMaster();
rstConn3.getDB("test").a.insert({a:3, str:"GREENEGGSANDHAM"});
assert.eq(3, rstConn3.getDB("test").a.count(), "Error interacting with replSet");

// Check that ssl connections can be made
var canConnectSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--ssl", "--eval", ";");
assert.eq(0, canConnectSSL, "SSL Connection attempt failed when it should succeed");
