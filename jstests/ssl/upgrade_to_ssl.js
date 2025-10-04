/**
 * This test checks the upgrade path for mixed mode ssl
 * from allowTLS up to requireTLS
 *
 * NOTE: This test is similar to upgrade_to_ssl_nossl.js in the
 * sslSpecial test suite. This test uses ssl communication
 * and therefore cannot test modes that do not allow ssl.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {CA_CERT, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

// "tlsAllowInvalidCertificates" is enabled to avoid hostname conflicts with our testing certs
let opts = {
    tlsMode: "allowTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsAllowInvalidCertificates: "",
    tlsAllowConnectionsWithoutCertificates: "",
    tlsCAFile: "jstests/libs/ca.pem",
};
let rst = new ReplSetTest({name: "tlsSet", nodes: 3, nodeOptions: opts});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let rstConn1 = rst.getPrimary();
rstConn1.getDB("test").a.insert({a: 1, str: "TESTTESTTEST"});
assert.eq(1, rstConn1.getDB("test").a.count(), "Error interacting with replSet");

print("===== UPGRADE allowTLS -> preferTLS =====");
opts.tlsMode = "preferTLS";
rst.upgradeSet(opts);
let rstConn2 = rst.getPrimary();
rstConn2.getDB("test").a.insert({a: 2, str: "CHECKCHECK"});
assert.eq(2, rstConn2.getDB("test").a.count(), "Error interacting with replSet");

// Check that non-ssl connections can still be made
let canConnectNoSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--eval", ";");
assert.eq(0, canConnectNoSSL, "non-SSL Connection attempt failed when it should succeed");

print("===== UPGRADE preferTLS -> requireTLS =====");
opts.tlsMode = "requireTLS";
rst.upgradeSet(opts);
let rstConn3 = rst.getPrimary();
rstConn3.getDB("test").a.insert({a: 3, str: "GREENEGGSANDHAM"});
assert.eq(3, rstConn3.getDB("test").a.count(), "Error interacting with replSet");

// Check that ssl connections can be made
let canConnectSSL = runMongoProgram("mongo", "--port", rst.ports[0], "--tls", "--tlsCAFile", CA_CERT, "--eval", ";");
assert.eq(0, canConnectSSL, "SSL Connection attempt failed when it should succeed");
rst.stopSet();
