// Check that OCSP verification works
// @tags: [requires_http_client]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (determineSSLProvider() != "openssl") {
    return;
}

if (!supportsStapling()) {
    return;
}

let mock_ocsp = new MockOCSPServer("", 20);
mock_ocsp.start();

const ocsp_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT,
    sslCAFile: OCSP_CA_CERT,
    sslAllowInvalidHostnames: "",
    setParameter: {
        "ocspEnabled": "true",
    },
};

let conn = null;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

mock_ocsp.stop();
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1000);
mock_ocsp.start();

// We're sleeping here to give the server enough time to fetch a new OCSP response
// saying that it's revoked.
sleep(15000);

assert.throws(() => {
    new Mongo(conn.host);
});

mock_ocsp.stop();
mock_ocsp = new MockOCSPServer("", 1000);
mock_ocsp.start();

// This ensures that the client was viewing a stapled response.
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