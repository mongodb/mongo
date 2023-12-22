// Check that rotation works for the CRL

import {copyCertificateFile, determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() === "apple") {
    quit();
}

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile("jstests/libs/crl.pem", dbPath + "/crl-test.pem");

const mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    tlsCRLFile: dbPath + "/crl-test.pem"
});

const host = "localhost:" + mongod.port;

// Make sure that client-revoked can connect at first
let out = runMongoProgram("mongo",
                          "--host",
                          host,
                          "--tls",
                          "--tlsCertificateKeyFile",
                          "jstests/libs/client_revoked.pem",
                          "--tlsCAFile",
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
                      "--tls",
                      "--tlsCertificateKeyFile",
                      "jstests/libs/client_revoked.pem",
                      "--tlsCAFile",
                      "jstests/libs/ca.pem",
                      "--eval",
                      ";");
assert.neq(out, 0, "Mongo invocation did not fail");

// Make sure client can still connect
out = runMongoProgram("mongo",
                      "--host",
                      host,
                      "--tls",
                      "--tlsCertificateKeyFile",
                      "jstests/libs/client.pem",
                      "--tlsCAFile",
                      "jstests/libs/ca.pem",
                      "--eval",
                      ";");
assert.eq(out, 0, "Mongo invocation failed");

MongoRunner.stopMongod(mongod);