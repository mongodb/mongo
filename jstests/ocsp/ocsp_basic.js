// Check that OCSP verification works
// @tags: [requires_http_client]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

var ocsp_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT,
    sslCAFile: OCSP_CA_CERT,
    sslAllowInvalidHostnames: "",
    setParameter: {
        "failpoint.disableStapling": "{'mode':'alwaysOn'}",
        "ocspEnabled": "true",
    },
};

let mock_ocsp = new MockOCSPServer("", 1);
mock_ocsp.start();

var conn = null;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

MongoRunner.stopMongod(conn);
mock_ocsp.stop();

// We need to test different certificates for revoked and not
// revoked on OSX, so we may as well run this test on all platforms.
Object.extend(ocsp_options, {waitForConnect: false});
ocsp_options.sslPEMKeyFile = OCSP_SERVER_CERT_REVOKED;

mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1);
mock_ocsp.start();

conn = MongoRunner.runMongod(ocsp_options);

waitForServer(conn);

assert.throws(() => {
    new Mongo(conn.host);
});

mock_ocsp.stop();
MongoRunner.stopMongod(conn);

// We have to search for the error code that SecureTransport emits when
// a certificate is revoked.
if (determineSSLProvider() === "apple") {
    const APPLE_OCSP_ERROR_CODE = "CSSMERR_TP_CERT_REVOKED";
    let output = rawMongoProgramOutput();
    assert(output.search(APPLE_OCSP_ERROR_CODE));
    return;
}

ocsp_options.sslPEMKeyFile = OCSP_SERVER_CERT;

clearOCSPCache();

mock_ocsp = new MockOCSPServer("", 1);
mock_ocsp.start();

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

mock_ocsp.stop();

// Test Scenario when Mock OCSP Server replies stating
// that the OCSP status of the client cert is revoked.
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1);
mock_ocsp.start();

assert.throws(() => {
    new Mongo(conn.host);
});

MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();
}());