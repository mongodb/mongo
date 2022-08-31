// Tests that OCSP responder latency is logged for client-side verification.
// @tags: [
//   requires_http_client,
//   live_record_incompatible,
// ]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

// We only have custom logging output for openssl.
if (determineSSLProvider() !== "openssl") {
    return;
}

const ocsp_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT,
    sslCAFile: OCSP_CA_PEM,
    sslAllowInvalidHostnames: "",
    setParameter: {
        "failpoint.disableStapling": "{'mode':'alwaysOn'}",
        "ocspEnabled": "true",
    },
};

let mock_ocsp = new MockOCSPServer("", 1);
mock_ocsp.start();

let conn = MongoRunner.runMongod(ocsp_options);

clearRawMongoProgramOutput();
// The desired log line will be printed by the shell. We run a parallel shell because
// 'rawMongoProgramOutput' will only return logs for subprocesses spawned by the shell.
const runParallelShellSuccess = startParallelShell(
    () => {
        jsTestLog(
            "Established connection with server to test successful certification verification.");
    },
    conn.port,
    null /*noConnect */,
    "--tls",
    "--tlsCAFile",
    OCSP_CA_PEM,
    "--tlsCertificateKeyFile",
    OCSP_CLIENT_CERT,
    "--tlsAllowInvalidHostnames",
    "--verbose",
    1);

runParallelShellSuccess();
let output = rawMongoProgramOutput();
assert.gte(output.search(/"id":6840101/), 0, output);

mock_ocsp.stop();

jsTestLog("Restarting MockOCSPServer with FAULT_REVOKED option");
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1);
mock_ocsp.start();

clearRawMongoProgramOutput();
jsTestLog("Spawning parallel shell that should throw due to revoked OCSP certificate");
assert.throws(startParallelShell(
    () => {
        jsTestLog("Something went wrong if we print this!");
    },
    conn.port,
    null /*noConnect */,
    "--tls",
    "--tlsCAFile",
    OCSP_CA_PEM,
    "--tlsCertificateKeyFile",
    OCSP_CLIENT_CERT,
    "--tlsAllowInvalidHostnames",
    "--verbose",
    1));

output = rawMongoProgramOutput();
// Assert that the shell fails due to certificate being revoked, and we still measure OCSP responder
// latency.
assert.gte(output.search(/OCSPCertificateStatusRevoked/), 0);
assert.gte(output.search(/"id":6840101/), 0);

MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();
}());