// Check that rotation works for the CRL

import {copyCertificateFile, determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() === "apple") {
    quit();
}

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile(getX509Path("crl.pem"), dbPath + "/crl-test.pem");

const mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
    tlsCRLFile: dbPath + "/crl-test.pem",
});

const host = "localhost:" + mongod.port;

// Make sure that client-revoked can connect at first
let out = runMongoProgram(
    "mongo",
    "--host",
    host,
    "--tls",
    "--tlsCertificateKeyFile",
    getX509Path("client_revoked.pem"),
    "--tlsCAFile",
    getX509Path("ca.pem"),
    "--eval",
    ";",
);
assert.eq(out, 0, "Initial mongo invocation failed");

// Rotate in new CRL
copyCertificateFile(getX509Path("crl_client_revoked.pem"), dbPath + "/crl-test.pem");

assert.commandWorked(mongod.adminCommand({rotateCertificates: 1}));

// Make sure client-revoked can't connect
out = runMongoProgram(
    "mongo",
    "--host",
    host,
    "--tls",
    "--tlsCertificateKeyFile",
    getX509Path("client_revoked.pem"),
    "--tlsCAFile",
    getX509Path("ca.pem"),
    "--eval",
    ";",
);
assert.neq(out, 0, "Mongo invocation did not fail");

// Make sure client can still connect
out = runMongoProgram(
    "mongo",
    "--host",
    host,
    "--tls",
    "--tlsCertificateKeyFile",
    getX509Path("client.pem"),
    "--tlsCAFile",
    getX509Path("ca.pem"),
    "--eval",
    ";",
);
assert.eq(out, 0, "Mongo invocation failed");

MongoRunner.stopMongod(mongod);
