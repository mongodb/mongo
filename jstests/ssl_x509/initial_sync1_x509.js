// Basic tests for cluster authentication using x509.

var common_options = {
    keyFile: "jstests/libs/key1",
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslAllowInvalidHostnames: ""
};

function runInitialSyncTest() {
    load("jstests/replsets/rslib.js");

    // The mongo shell cannot authenticate as the internal __system user in tests that use x509 for
    // cluster authentication. Choosing the default value for wcMajorityJournalDefault in
    // ReplSetTest cannot be done automatically without the shell performing such authentication, so
    // in this test we must make the choice explicitly, based on the global test options.
    var wcMajorityJournalDefault;
    if (jsTestOptions().storageEngine == "inMemory") {
        wcMajorityJournalDefault = false;
    } else {
        wcMajorityJournalDefault = true;
    }
    print("1. Bring up set");
    var replTest = new ReplSetTest({
        name: "jstests_initsync1_x509",
        nodes: {node0: x509_options1, node1: x509_options2},
        waitForKeys: false
    });
    var conns = replTest.startSet();

    replTest.initiate();

    var primary = replTest.getPrimary();
    var foo = primary.getDB("foo");
    var admin = primary.getDB("admin");

    var secondary1 = replTest.getSecondary();

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
    for (var i = 0; i < 100; i++) {
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
    common_options, {sslClusterFile: "jstests/libs/cluster_cert.pem", clusterAuthMode: "x509"});
var x509_options2 = x509_options1;
runInitialSyncTest();

// Mixed clusterAuthMode: sendX509 and sendKeyFile and try adding --auth
x509_options1 = Object.merge(
    common_options,
    {sslClusterFile: "jstests/libs/cluster_cert.pem", clusterAuthMode: "sendX509", auth: ""});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendKeyFile"});
runInitialSyncTest();

// Mixed clusterAuthMode: x509 and sendX509, use the PEMKeyFile for outgoing connections
x509_options1 = Object.merge(common_options, {clusterAuthMode: "x509"});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendX509"});
runInitialSyncTest();

// verify that replset initiate fails if using a self-signed cert
x509_options1 = Object.merge(common_options, {clusterAuthMode: "x509"});
x509_options2 = Object.merge(common_options,
                             {sslClusterFile: "jstests/libs/smoke.pem", clusterAuthMode: "x509"});
var replTest = new ReplSetTest({nodes: {node0: x509_options1, node1: x509_options2}});

// We don't want to invoke the hang analyzer because we
// expect this test to fail by timing out
MongoRunner.runHangAnalyzer.disable();

var conns = replTest.startSet();
assert.throws(function() {
    replTest.initiate();
});

// stopSet will also fail because we cannot authenticate to stop it properly.
// Ignore the error around unterminated processes.
TestData.failIfUnterminatedProcesses = false;

assert.throws(function() {
    replTest.stopSet();
});
