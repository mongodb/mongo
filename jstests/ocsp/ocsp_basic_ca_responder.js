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
    OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
    OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM,
    OCSP_SERVER_CERT,
    OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM
} from "jstests/ocsp/lib/ocsp_helpers.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() === "apple") {
    quit();
}
function test(serverCert, caCert, responderCertPair) {
    clearOCSPCache();

    const ocsp_options = {
        sslMode: "requireSSL",
        sslPEMKeyFile: serverCert,
        sslCAFile: caCert,
        sslAllowInvalidHostnames: "",
        setParameter: {
            "failpoint.disableStapling": "{'mode':'alwaysOn'}",
            "ocspEnabled": "true",
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

    mock_ocsp.stop();
    mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1, responderCertPair);
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
}

test(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_CA_RESPONDER);
test(OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
     OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
     OCSP_INTERMEDIATE_RESPONDER);
test(OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM, OCSP_CA_PEM, OCSP_INTERMEDIATE_RESPONDER);
