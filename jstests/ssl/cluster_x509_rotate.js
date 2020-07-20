// Check that rotation works for the cluster certificate

(function() {
"use strict";

load('jstests/ssl/libs/ssl_helpers.js');

if (determineSSLProvider() === "openssl") {
    return;
}

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile("jstests/libs/ca.pem", dbPath + "/ca-test.pem");
copyCertificateFile("jstests/libs/client.pem", dbPath + "/client-test.pem");
copyCertificateFile("jstests/libs/server.pem", dbPath + "/server-test.pem");

// Make replset with old certificates, rotate to new certificates, and try to add
// a node with new certificates.
const rst = new ReplSetTest({nodes: 2});
rst.startSet({
    sslMode: "requireSSL",
    sslPEMKeyFile: dbPath + "/server-test.pem",
    sslCAFile: dbPath + "/ca-test.pem",
    sslClusterFile: dbPath + "/client-test.pem",
    sslAllowInvalidHostnames: "",
});

rst.initiate();
rst.awaitReplication();

copyCertificateFile("jstests/libs/trusted-ca.pem", dbPath + "/ca-test.pem");
copyCertificateFile("jstests/libs/trusted-client.pem", dbPath + "/client-test.pem");
copyCertificateFile("jstests/libs/trusted-server.pem", dbPath + "/server-test.pem");

for (let node of rst.nodes) {
    assert.commandWorked(node.adminCommand({rotateCertificates: 1}));
}

const newnode = rst.add({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/trusted-server.pem",
    sslCAFile: "jstests/libs/trusted-ca.pem",
    sslClusterFile: "jstests/libs/trusted-client.pem",
    sslAllowInvalidHostnames: "",
    waitForConnect: false,
});

// Emulate waitForConnect so we wait for new node to come up before killing rst
const host = "localhost:" + newnode.port;
assert.soon(() => {
    return 0 ===
        runMongoProgram("mongo",
                        "--ssl",
                        "--sslAllowInvalidHostnames",
                        "--host",
                        host,
                        "--sslPEMKeyFile",
                        "jstests/libs/trusted-client.pem",
                        "--sslCAFile",
                        "jstests/libs/trusted-ca.pem",
                        "--eval",
                        ";");
});

rst.reInitiate();

// Make sure each node can connect to each other node
for (let node of rst.nodeList()) {
    for (let target of rst.nodeList()) {
        if (node !== target) {
            assert.eq(0,
                      runMongoProgram(
                          "mongo",
                          "--ssl",
                          "--sslAllowInvalidHostnames",
                          "--host",
                          node,
                          "--sslPEMKeyFile",
                          "jstests/libs/trusted-client.pem",
                          "--sslCAFile",
                          "jstests/libs/trusted-ca.pem",
                          "--eval",
                          `assert.commandWorked(db.adminCommand({replSetTestEgress: 1, target: "${
                              target}"}));`));
        }
    }
}

rst.stopSet();
}());
