/**
 * This test checks the upgrade path for mixed mode ssl
 * from disabled up to preferSSL
 *
 * NOTE: This test is similar to upgrade_to_ssl.js in the
 * ssl test suite. This test cannot use ssl communication
 * and therefore cannot test modes that only allow ssl.
 */

load("jstests/ssl/libs/ssl_helpers.js");

var rst = new ReplSetTest({name: 'sslSet', nodes: 3, nodeOptions: {sslMode: "disabled"}});
rst.startSet();
rst.initiate();

var rstConn1 = rst.getPrimary();
rstConn1.getDB("test").a.insert({a: 1, str: "TESTTESTTEST"});
assert.eq(1, rstConn1.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE disabled -> allowSSL =====");
rst.upgradeSet({sslMode: "allowSSL", sslPEMKeyFile: SERVER_CERT, sslAllowInvalidCertificates: ""});
var rstConn2 = rst.getPrimary();
rstConn2.getDB("test").a.insert({a: 2, str: "TESTTESTTEST"});
assert.eq(2, rstConn2.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE allowSSL -> preferSSL =====");
rst.upgradeSet({sslMode: "preferSSL", sslPEMKeyFile: SERVER_CERT, sslAllowInvalidCertificates: ""});
var rstConn3 = rst.getPrimary();
rstConn3.getDB("test").a.insert({a: 3, str: "TESTTESTTEST"});
assert.eq(3, rstConn3.getDB("test").a.count(), "Error interacting with replSet");

var canConnectSSL = runMongoProgram(
    "mongo", "--port", rst.ports[0], "--ssl", "--sslAllowInvalidCertificates", "--eval", ";");
assert.eq(0, canConnectSSL, "SSL Connection attempt failed when it should succeed");
