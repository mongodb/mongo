/**
 * This test checks the upgrade path for mixed mode ssl + x509 auth
 * from disabled/keyfiles up to preferSSL/x509
 *
 * NOTE: This test is similar to upgrade_to_x509_ssl.js in the
 * ssl test suite. This test cannot use ssl communication
 * and therefore cannot test modes that only allow ssl.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

load("jstests/ssl/libs/ssl_helpers.js");

(function() {
'use strict';

// The mongo shell cannot authenticate as the internal __system user in tests that use x509 for
// cluster authentication. Choosing the default value for wcMajorityJournalDefault in
// ReplSetTest cannot be done automatically without the shell performing such authentication, so
// in this test we must make the choice explicitly, based on the global test options.
const wcMajorityJournalDefault = jsTestOptions().storageEngine != "inMemory";

const opts = {
    sslMode: "disabled",
    clusterAuthMode: "keyFile",
};
const NUM_NODES = 3;
const rst = new ReplSetTest(
    {name: 'sslSet', nodes: NUM_NODES, waitForKeys: false, keyFile: KEYFILE, nodeOptions: opts});
rst.startSet();

// ReplSetTest.initiate() requires all nodes to be to be authorized to run replSetGetStatus.
// TODO(SERVER-14017): Remove this in favor of using initiate() everywhere.
rst.initiateWithAnyNodeAsPrimary(Object.extend(
    rst.getReplSetConfig(), {writeConcernMajorityJournalDefault: wcMajorityJournalDefault}));

// Make administrative user other than local.__system
rst.getPrimary().getDB("admin").createUser({user: "root", pwd: "pwd", roles: ["root"]},
                                           {w: NUM_NODES});

let entriesWritten = 0;
function testWrite(str) {
    const entry = ++entriesWritten;

    const conn = rst.getPrimary();
    assert(conn.getDB('admin').auth('root', 'pwd'));
    const test = conn.getDB('test');
    assert.writeOK(test.a.insert({a: entry, str: str}));
    assert.eq(entry, test.a.find().itcount(), "Error interacting with replSet");
}

function authAllNodes(nodes) {
    for (let n = 0; n < nodes.length; n++) {
        assert(rst.nodes[n].getDB("admin").auth("root", "pwd"));
    }
}

function upgradeAndWrite(newOpts, str) {
    authAllNodes(rst.nodes);
    rst.upgradeSet(newOpts, 'root', 'pwd');
    authAllNodes(rst.nodes);
    rst.awaitReplication();
    testWrite(str);
}

function upgradeWriteAndConnect(newOpts, str) {
    upgradeAndWrite(newOpts, str);

    assert.eq(0,
              runMongoProgram("mongo",
                              "--port",
                              rst.ports[0],
                              "--ssl",
                              "--sslAllowInvalidCertificates",
                              "--sslPEMKeyFile",
                              CLIENT_CERT,
                              "--eval",
                              ";"),
              "SSL Connection attempt failed when it should succeed");
}

testWrite(rst.getPrimary(), 'TESTTESTTEST');

jsTest.log("===== UPGRADE disabled,keyFile -> allowSSL,sendKeyfile =====");
upgradeAndWrite({
    sslMode: "allowSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslAllowInvalidCertificates: "",
    clusterAuthMode: "sendKeyFile",
    keyFile: KEYFILE,
    sslCAFile: CA_CERT
},
                'CHECKCHECKCHECK');

jsTest.log("===== UPGRADE allowSSL,sendKeyfile -> preferSSL,sendX509 =====");
upgradeWriteAndConnect({
    sslMode: "preferSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslAllowInvalidCertificates: "",
    clusterAuthMode: "sendX509",
    keyFile: KEYFILE,
    sslCAFile: CA_CERT
},
                       'PEASandCARROTS');

jsTest.log("===== UPGRADE preferSSL,sendX509 -> preferSSL,x509 =====");
// we cannot upgrade past preferSSL here because it will break the test client
upgradeWriteAndConnect({
    sslMode: "preferSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslAllowInvalidCertificates: "",
    clusterAuthMode: "x509",
    keyFile: KEYFILE,
    sslCAFile: CA_CERT
},
                       'BEEP BOOP');

rst.stopSet();
})();
