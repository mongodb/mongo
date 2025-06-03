// Basic tests for cluster authentication using x509.

import {ReplSetTest} from "jstests/libs/replsettest.js";

var common_options = {
    keyFile: "jstests/libs/key1",
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    tlsAllowInvalidHostnames: ""
};

function runInitialSyncTest() {
    print("1. Bring up set");
    var replTest = new ReplSetTest({
        name: "jstests_initsync1_x509",
        nodes: {node0: x509_options1, node1: x509_options2},
        waitForKeys: false
    });
    replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var foo = primary.getDB("foo");
    var admin = primary.getDB("admin");

    print("2. Create a root user.");
    admin.createUser({user: "root", pwd: "pass", roles: ["root"]});
    authutil.assertAuthenticate(replTest.getPrimary(), '$external', {
        mechanism: 'MONGODB-X509',
    });

    print("3. Insert some data");
    var bulk = foo.bar.initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({date: new Date(), x: i, str: "all the talk on the market"});
    }
    assert.commandWorked(bulk.execute());
    print("total in foo: " + foo.bar.count());

    print("4. Make sure synced");
    replTest.awaitReplication();

    print("5. Insert some stuff");
    primary = replTest.getPrimary();
    bulk = foo.bar.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; i++) {
        bulk.insert({date: new Date(), x: i, str: "all the talk on the market"});
    }
    assert.commandWorked(bulk.execute());

    print("6. Everyone happy eventually");
    replTest.awaitReplication(300000);

    admin.logout();
    replTest.stopSet();
}

// Standard case, clusterAuthMode: x509
var x509_options1 = Object.merge(
    common_options, {tlsClusterFile: "jstests/libs/cluster_cert.pem", clusterAuthMode: "x509"});
var x509_options2 = x509_options1;
runInitialSyncTest();

// Mixed clusterAuthMode: sendX509 and sendKeyFile and try adding --auth
x509_options1 = Object.merge(
    common_options,
    {tlsClusterFile: "jstests/libs/cluster_cert.pem", clusterAuthMode: "sendX509", auth: ""});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendKeyFile"});
runInitialSyncTest();

// Mixed clusterAuthMode: x509 and sendX509, use the PEMKeyFile for outgoing connections
x509_options1 = Object.merge(common_options, {clusterAuthMode: "x509"});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendX509"});
runInitialSyncTest();
