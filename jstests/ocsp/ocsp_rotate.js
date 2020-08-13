// Validate rotate certificates works with ocsp
(function() {
"use strict";

load('jstests/ssl/libs/ssl_helpers.js');
load('jstests/ocsp/lib/mock_ocsp.js');

if (determineSSLProvider() !== "openssl") {
    return;
}

let mongod;

// Returns whether a rotation works with the given mockOCSP server.
function tryRotate(fault) {
    const ocspServer = new MockOCSPServer(fault);
    ocspServer.start();

    const success = mongod.adminCommand({rotateCertificates: 1}).ok;

    ocspServer.stop();

    return success;
}

mongod = MongoRunner.runMongod(
    {sslMode: "requireSSL", sslPEMKeyFile: OCSP_SERVER_CERT, sslCAFile: OCSP_CA_PEM});

// Positive: test with positive OCSP response
assert(tryRotate());

// Negative: test with revoked OCSP response
assert(!tryRotate(FAULT_REVOKED));

// Positive: test with positive OCSP response
assert(tryRotate());
}());
