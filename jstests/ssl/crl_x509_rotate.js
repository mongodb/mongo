// Check that rotation works for the CRL

(function() {
"use strict";

load('jstests/ssl/libs/ssl_helpers.js');

if (determineSSLProvider() === "apple") {
    return;
}

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile("jstests/libs/crl.pem", dbPath + "/crl-test.pem");

const mongod = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslCRLFile: dbPath + "/crl-test.pem"
});

const host = "localhost:" + mongod.port;

// Make sure that client-revoked can connect at first
let out = runMongoProgram("mongo",
                          "--host",
                          host,
                          "--ssl",
                          "--sslPEMKeyFile",
                          "jstests/libs/client_revoked.pem",
                          "--sslCAFile",
                          "jstests/libs/ca.pem",
                          "--eval",
                          ";");
assert.eq(out, 0, "Initial mongo invocation failed");

// Rotate in new CRL
copyCertificateFile("jstests/libs/crl_client_revoked.pem", dbPath + "/crl-test.pem");

assert.commandWorked(mongod.adminCommand({rotateCertificates: 1}));

// Make sure client-revoked can't connect
out = runMongoProgram("mongo",
                      "--host",
                      host,
                      "--ssl",
                      "--sslPEMKeyFile",
                      "jstests/libs/client_revoked.pem",
                      "--sslCAFile",
                      "jstests/libs/ca.pem",
                      "--eval",
                      ";");
assert.neq(out, 0, "Mongo invocation did not fail");

// Make sure client can still connect
out = runMongoProgram("mongo",
                      "--host",
                      host,
                      "--ssl",
                      "--sslPEMKeyFile",
                      "jstests/libs/client.pem",
                      "--sslCAFile",
                      "jstests/libs/ca.pem",
                      "--eval",
                      ";");
assert.eq(out, 0, "Mongo invocation failed");

MongoRunner.stopMongod(mongod);
}());
