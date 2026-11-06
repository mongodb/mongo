// Check that OCSP verification works
// @tags: [requires_http_client]

import {
    FAULT_REVOKED,
    MockOCSPServer,
    OCSP_CA_RESPONDER,
    OCSP_INTERMEDIATE_RESPONDER,
} from "jstests/ocsp/lib/mock_ocsp.js";
import {
    clearOCSPCache,
    OCSP_CA_PEM,
    OCSP_CLIENT_CERT,
    OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
    OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM,
    OCSP_SERVER_CERT,
    OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
} from "jstests/ocsp/lib/ocsp_helpers.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() === "apple") {
    quit();
}

// The default (4s) can be exceeded by a slow revocation check, which soft-fails and accepts a
// revoked certificate.
const kOCSPVerifyTimeoutSecs = 10;

// Connect from a child shell, since the OCSP verify timeout can only be set at shell startup.
// Returns the child shell's exit code.
function connectWithOCSPTimeout(conn) {
    const pid = startMongoProgramNoConnect(
        MongoRunner.getMongoShellPath(),
        "--host",
        conn.host,
        "--tls",
        "--tlsCAFile",
        OCSP_CA_PEM,
        "--tlsCertificateKeyFile",
        OCSP_CLIENT_CERT,
        "--tlsAllowInvalidHostnames",
        "--setShellParameter",
        "ocspEnabled=true",
        "--setShellParameter",
        `tlsOCSPVerifyTimeoutSecs=${kOCSPVerifyTimeoutSecs}`,
        "--eval",
        "assert.commandWorked(db.adminCommand({hello: 1}));",
    );
    return waitProgram(pid);
}

function test(serverCert, caCert, responderCertPair) {
    clearOCSPCache();

    const ocsp_options = {
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: serverCert,
        tlsCAFile: caCert,
        tlsAllowInvalidHostnames: "",
        setParameter: {
            "failpoint.disableStapling": "{'mode':'alwaysOn'}",
            "ocspEnabled": "true",
            "tlsOCSPVerifyTimeoutSecs": kOCSPVerifyTimeoutSecs,
        },
    };

    // This is to test what happens when the responder is down,
    // making sure that we soft fail.
    let conn = null;

    let mock_ocsp = new MockOCSPServer("", 1, responderCertPair);
    mock_ocsp.start();

    assert.doesNotThrow(() => {
        conn = MongoRunner.runMongod(ocsp_options);
    });

    // Check that the child shell connects while the certificate is good, so that a failure below
    // can only be caused by the revoked status.
    assert.eq(0, connectWithOCSPTimeout(conn), "Expected connection with a good certificate");

    mock_ocsp.stop();
    mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1, responderCertPair);
    mock_ocsp.start();

    assert.neq(
        0,
        connectWithOCSPTimeout(conn),
        "Expected connection with a revoked certificate to fail",
    );

    MongoRunner.stopMongod(conn);

    // The mongoRunner spawns a new Mongo Object to validate the collections which races
    // with the shutdown logic of the mock_ocsp responder on some platforms. We need this
    // sleep to make sure that the threads don't interfere with each other.
    sleep(1000);
    mock_ocsp.stop();
}

test(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_CA_RESPONDER);
test(
    OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
    OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
    OCSP_INTERMEDIATE_RESPONDER,
);
test(OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM, OCSP_CA_PEM, OCSP_INTERMEDIATE_RESPONDER);
