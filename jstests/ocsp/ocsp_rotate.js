// Validate rotate certificates works with ocsp
// @tags: [requires_http_client, requires_ocsp_stapling]
import {FAULT_REVOKED, MockOCSPServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {OCSP_CA_PEM, OCSP_SERVER_CERT, supportsStapling} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
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
