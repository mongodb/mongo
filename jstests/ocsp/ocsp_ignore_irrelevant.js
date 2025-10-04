// Check that OCSP ignores statuses of irrelevant certificates in the response
// @tags: [requires_http_client, requires_ocsp_stapling]

import {
    FAULT_REVOKED,
    MockOCSPServer,
    OCSP_CA_RESPONDER,
    OCSP_DELEGATE_RESPONDER,
    OCSP_INTERMEDIATE_RESPONDER,
} from "jstests/ocsp/lib/mock_ocsp.js";
import {
    assertClientConnectFails,
    assertClientConnectSucceeds,
    clearOCSPCache,
    OCSP_CA_PEM,
    OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
    OCSP_REVOKED,
    OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM,
    OCSP_SERVER_CERT,
    OCSP_SERVER_MUSTSTAPLE_CERT,
    OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
    supportsStapling,
    waitForServer,
} from "jstests/ocsp/lib/ocsp_helpers.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() === "apple") {
    quit();
}

const INCLUDE_EXTRA_STATUS = true;

/**
 * Tests OCSP status verification in the client-side ignores the statuses
 * of irrelevant certificates. No stapling is performed server-side.
 */
function testClient(serverCert, caCert, responderCertPair, issuerDigest) {
    jsTestLog("Running client test with params: " + JSON.stringify(arguments));

    clearOCSPCache();

    let mock_ocsp = new MockOCSPServer("", 1, responderCertPair, 0, INCLUDE_EXTRA_STATUS, issuerDigest);

    let ocsp_options = {
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: serverCert,
        tlsCAFile: caCert,
        tlsAllowInvalidHostnames: "",
        setParameter: {
            "failpoint.disableStapling": "{'mode':'alwaysOn'}",
            "ocspEnabled": "true",
        },
        waitForConnect: false,
    };

    const conn = MongoRunner.runMongod(ocsp_options);
    waitForServer(conn);

    jsTestLog("Testing client can connect if OCSP response has extraneous statuses and the matching CertID is Good");
    mock_ocsp.start();

    assertClientConnectSucceeds(conn);

    mock_ocsp.stop();

    jsTestLog(
        "Testing client can't connect if OCSP response has extraneous statuses and the matching CertID is Revoked",
    );
    mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1, responderCertPair, 0, INCLUDE_EXTRA_STATUS, issuerDigest);
    mock_ocsp.start();

    assertClientConnectFails(conn);

    MongoRunner.stopMongod(conn);

    // The mongoRunner spawns a new Mongo Object to validate the collections which races
    // with the shutdown logic of the mock_ocsp responder on some platforms. We need this
    // sleep to make sure that the threads don't interfere with each other.
    sleep(1000);
    mock_ocsp.stop();
}

/**
 * Tests OCSP status verification in the server-side (for stapling) ignores
 * the statuses of irrelevant certificates. The server certificate must have
 * the status_request TLS Feature (aka MustStaple) extension for the assertions
 * in this test to work.
 */
function testStapling(serverCert, caCert, responderCertPair, issuerDigest) {
    jsTestLog("Running stapling test with params: " + JSON.stringify(arguments));

    clearOCSPCache();

    let mock_ocsp = new MockOCSPServer("", 32400, responderCertPair, 0, INCLUDE_EXTRA_STATUS, issuerDigest);

    let ocsp_options = {
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: serverCert,
        tlsCAFile: caCert,
        tlsAllowInvalidHostnames: "",
        setParameter: {
            "ocspEnabled": "true",
        },
        waitForConnect: false,
    };

    let conn = null;

    jsTestLog(
        "Testing server staples a Good status if OCSP response has extraneous statuses and the matching CertID is Good",
    );
    mock_ocsp.start();

    conn = MongoRunner.runMongod(ocsp_options);
    waitForServer(conn);

    assertClientConnectSucceeds(conn);

    MongoRunner.stopMongod(conn);
    sleep(1000);
    mock_ocsp.stop();

    jsTestLog(
        "Testing server staples a revoked status if OCSP response has extraneous statuses and the matching CertID is Revoked",
    );
    Object.extend(ocsp_options, {waitForConnect: false});
    mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 32400, responderCertPair, 0, INCLUDE_EXTRA_STATUS, issuerDigest);
    mock_ocsp.start();

    conn = MongoRunner.runMongod(ocsp_options);
    waitForServer(conn);

    assertClientConnectFails(conn, OCSP_REVOKED);

    MongoRunner.stopMongod(conn);

    // The mongoRunner spawns a new Mongo Object to validate the collections which races
    // with the shutdown logic of the mock_ocsp responder on some platforms. We need this
    // sleep to make sure that the threads don't interfere with each other.
    sleep(1000);
    mock_ocsp.stop();
}

let digests = ["sha1"];
if (determineSSLProvider() !== "windows") {
    // windows can't handle issuer names & keys hashed
    // using sha256, so this is only tested on openssl.
    digests.push("sha256");
}

for (const digest of digests) {
    testClient(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_DELEGATE_RESPONDER, digest);
    testClient(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_CA_RESPONDER, digest);
    testClient(
        OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
        OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
        OCSP_INTERMEDIATE_RESPONDER,
        digest,
    );
    testClient(OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM, OCSP_CA_PEM, OCSP_INTERMEDIATE_RESPONDER, digest);
}

if (!supportsStapling()) {
    quit();
}

for (const digest of digests) {
    testStapling(OCSP_SERVER_MUSTSTAPLE_CERT, OCSP_CA_PEM, OCSP_DELEGATE_RESPONDER, digest);
}
