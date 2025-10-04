/**
 * This test checks the upgrade path for mixed mode ssl + x509 auth
 * from disabled/keyfiles up to preferTLS/x509
 *
 * NOTE: This test is similar to upgrade_to_x509_ssl.js in the
 * ssl test suite. This test cannot use ssl communication
 * and therefore cannot test modes that only allow ssl.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {CA_CERT, CLIENT_CERT, KEYFILE, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

// The mongo shell cannot authenticate as the internal __system user in tests that use x509 for
// cluster authentication. Choosing the default value for wcMajorityJournalDefault in
// ReplSetTest cannot be done automatically without the shell performing such authentication, so
// in this test we must make the choice explicitly, based on the global test options.
const wcMajorityJournalDefault = jsTestOptions().storageEngine != "inMemory";

const opts = {
    tlsMode: "disabled",
    clusterAuthMode: "keyFile",
};
const NUM_NODES = 3;
const rst = new ReplSetTest({
    name: "tlsSet",
    nodes: NUM_NODES,
    waitForKeys: false,
    keyFile: KEYFILE,
    nodeOptions: opts,
});
rst.startSet();
rst.initiate(
    Object.extend(rst.getReplSetConfig(), {
        writeConcernMajorityJournalDefault: wcMajorityJournalDefault,
    }),
    null,
    {allNodesAuthorizedToRunRSGetStatus: false},
);

// Make administrative user other than local.__system
rst.getPrimary()
    .getDB("admin")
    .createUser({user: "root", pwd: "pwd", roles: ["root"]}, {w: NUM_NODES});

let entriesWritten = 0;
function testWrite(str) {
    const entry = ++entriesWritten;

    const conn = rst.getPrimary();
    assert(conn.getDB("admin").auth("root", "pwd"));
    const test = conn.getDB("test");
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
    rst.upgradeSet(newOpts, "root", "pwd");
    authAllNodes(rst.nodes);
    rst.awaitReplication();
    testWrite(str);
}

function upgradeWriteAndConnect(newOpts, str) {
    upgradeAndWrite(newOpts, str);

    assert.eq(
        0,
        runMongoProgram(
            "mongo",
            "--port",
            rst.ports[0],
            "--ssl",
            "--tlsCAFile",
            CA_CERT,
            "--tlsCertificateKeyFile",
            CLIENT_CERT,
            "--eval",
            ";",
        ),
        "SSL Connection attempt failed when it should succeed",
    );
}

testWrite(rst.getPrimary(), "TESTTESTTEST");

jsTest.log("===== UPGRADE disabled,keyFile -> allowTLS,sendKeyfile =====");
upgradeAndWrite(
    {
        tlsMode: "allowTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsAllowInvalidCertificates: "",
        clusterAuthMode: "sendKeyFile",
        keyFile: KEYFILE,
        tlsCAFile: CA_CERT,
    },
    "CHECKCHECKCHECK",
);

jsTest.log("===== UPGRADE allowTLS,sendKeyfile -> preferTLS,sendX509 =====");
upgradeWriteAndConnect(
    {
        tlsMode: "preferTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsAllowInvalidCertificates: "",
        clusterAuthMode: "sendX509",
        keyFile: KEYFILE,
        tlsCAFile: CA_CERT,
    },
    "PEASandCARROTS",
);

jsTest.log("===== UPGRADE preferTLS,sendX509 -> preferTLS,x509 =====");
// we cannot upgrade past preferTLS here because it will break the test client
upgradeWriteAndConnect(
    {
        tlsMode: "preferTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsAllowInvalidCertificates: "",
        clusterAuthMode: "x509",
        keyFile: KEYFILE,
        tlsCAFile: CA_CERT,
    },
    "BEEP BOOP",
);

rst.stopSet();
