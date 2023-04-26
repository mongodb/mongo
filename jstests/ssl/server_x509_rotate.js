// Check that rotation works for the server certificate

(function() {
"use strict";

load('jstests/ssl/libs/ssl_helpers.js');

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile("jstests/libs/ca.pem", dbPath + "/ca-test.pem");
copyCertificateFile("jstests/libs/server.pem", dbPath + "/server-test.pem");

const mongod = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: dbPath + "/server-test.pem",
    sslCAFile: dbPath + "/ca-test.pem"
});

// Rotate in new certificates
copyCertificateFile("jstests/libs/trusted-ca.pem", dbPath + "/ca-test.pem");
copyCertificateFile("jstests/libs/trusted-server.pem", dbPath + "/server-test.pem");

assert.commandWorked(mongod.getDB("test").rotateCertificates("Rotated!"));
// make sure that mongo is still connected after rotation
assert.commandWorked(mongod.adminCommand({connectionStatus: 1}));

const host = "localhost:" + mongod.port;

// Start shell with old certificates and make sure it can't connect
let out = runMongoProgram("mongo",
                          "--host",
                          host,
                          "--ssl",
                          "--sslPEMKeyFile",
                          "jstests/libs/client.pem",
                          "--sslCAFile",
                          "jstests/libs/ca.pem",
                          "--eval",
                          ";");
assert.neq(out, 0, "Mongo invocation did not fail");

// Start shell with new certificates and make sure it can connect
out = runMongoProgram("mongo",
                      "--host",
                      host,
                      "--ssl",
                      "--sslPEMKeyFile",
                      "jstests/libs/trusted-client.pem",
                      "--sslCAFile",
                      "jstests/libs/trusted-ca.pem",
                      "--eval",
                      ";");
assert.eq(out, 0, "Mongo invocation failed");

MongoRunner.stopMongod(mongod);
}());
