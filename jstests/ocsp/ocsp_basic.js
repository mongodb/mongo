// Check that OCSP verification works
// @tags: [
//   requires_http_client,
// ]

import {FAULT_REVOKED, MockOCSPServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {
    clearOCSPCache,
    OCSP_CA_PEM,
    OCSP_SERVER_CERT,
    OCSP_SERVER_CERT_REVOKED,
    waitForServer,
} from "jstests/ocsp/lib/ocsp_helpers.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

let ocsp_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: OCSP_SERVER_CERT,
    tlsCAFile: OCSP_CA_PEM,
    tlsAllowInvalidHostnames: "",
    setParameter: {
        "failpoint.disableStapling": "{'mode':'alwaysOn'}",
        "ocspEnabled": "true",
    },
};

let mock_ocsp = new MockOCSPServer("", 1);
mock_ocsp.start();

let conn = null;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

MongoRunner.stopMongod(conn);
mock_ocsp.stop();

// We need to test different certificates for revoked and not
// revoked on OSX, so we may as well run this test on all platforms.
Object.extend(ocsp_options, {waitForConnect: false});
ocsp_options.tlsCertificateKeyFile = OCSP_SERVER_CERT_REVOKED;

print("Restarting MockOCSPServer with FAULT_REVOKED option");
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1);
mock_ocsp.start();

conn = MongoRunner.runMongod(ocsp_options);

waitForServer(conn);

assert.throws(() => {
    print("Following connection should fail");
    new Mongo(conn.host);
});

mock_ocsp.stop();
MongoRunner.stopMongod(conn);

// We have to search for the error code that SecureTransport emits when
// a certificate is revoked.
if (determineSSLProvider() === "apple") {
    const APPLE_OCSP_ERROR_CODE = "CSSMERR_TP_CERT_REVOKED";
    let output = rawMongoProgramOutput(".*");
    assert(output.search(APPLE_OCSP_ERROR_CODE));
    quit();
}

clearOCSPCache();

// Give time for the OCSP cache to clean up.
sleep(1000);

// Test that soft fail works.
ocsp_options.tlsCertificateKeyFile = OCSP_SERVER_CERT;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

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
