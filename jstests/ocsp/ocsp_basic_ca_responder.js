// Check that OCSP verification works
// @tags: [requires_http_client]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (determineSSLProvider() === "apple") {
    return;
}

clearOCSPCache();

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

// This is to test what happens when the responder is down,
// making sure that we soft fail.
let conn = null;

let mock_ocsp = new MockOCSPServer("", 1, true);
mock_ocsp.start();

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

mock_ocsp.stop();
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1, true);
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