/**
 * This test checks the upgrade path for mixed mode ssl
 * from disabled up to preferTLS
 *
 * NOTE: This test is similar to upgrade_to_ssl.js in the
 * ssl test suite. This test cannot use ssl communication
 * and therefore cannot test modes that only allow ssl.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {CA_CERT, CLIENT_CERT, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

var rst = new ReplSetTest({
    name: 'tlsSet',
    nodes: [
        {},
        {},
        {rsConfig: {priority: 0}},
    ],
    nodeOptions: {
        tlsMode: "disabled",
    }
});
rst.startSet();
rst.initiate();

var rstConn1 = rst.getPrimary();
rstConn1.getDB("test").a.insert({a: 1, str: "TESTTESTTEST"});
assert.eq(1, rstConn1.getDB("test").a.find().itcount(), "Error interacting with replSet");

print("===== UPGRADE disabled -> allowTLS =====");
rst.upgradeSet({
    tlsMode: "allowTLS",
    tlsCAFile: CA_CERT,
    tlsCertificateKeyFile: SERVER_CERT,
    tlsAllowInvalidHostnames: "",
});
var rstConn2 = rst.getPrimary();
rstConn2.getDB("test").a.insert({a: 2, str: "TESTTESTTEST"});
assert.eq(2, rstConn2.getDB("test").a.find().itcount(), "Error interacting with replSet");

print("===== UPGRADE allowTLS -> preferTLS =====");
rst.upgradeSet({
    tlsMode: "preferTLS",
    tlsCAFile: CA_CERT,
    tlsCertificateKeyFile: SERVER_CERT,
});
var rstConn3 = rst.getPrimary();
rstConn3.getDB("test").a.insert({a: 3, str: "TESTTESTTEST"});
assert.eq(3, rstConn3.getDB("test").a.find().itcount(), "Error interacting with replSet");

print("===== Ensure SSL Connectable =====");
var canConnectSSL = runMongoProgram("mongo",
                                    "--port",
                                    rst.ports[0],
                                    "--ssl",
                                    '--tlsCAFile',
                                    CA_CERT,
                                    '--tlsCertificateKeyFile',
                                    CLIENT_CERT,
                                    "--eval",
                                    ";");
assert.eq(0, canConnectSSL, "SSL Connection attempt failed when it should succeed");
rst.stopSet();
