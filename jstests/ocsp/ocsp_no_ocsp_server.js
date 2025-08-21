// Check that attempt at OCSP verification when the OCSP server is not running. The
// MongoDB server should not throw an exception. The MongoDB server should also
// correctly handle transitioning from certificates with OCSP to ones without
// @tags: [
//   requires_http_client,
// ]

import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    clearOCSPCache,
    OCSP_CA_PEM,
    OCSP_NO_OCSP_SERVER_CERT,
    OCSP_SERVER_CERT,
} from "jstests/ocsp/lib/ocsp_helpers.js";
import {copyCertificateFile} from "jstests/ssl/libs/ssl_helpers.js";

// dataDir is defined in jstest.py
const dbPath = MongoRunner.toRealDir("$dataDir");
mkdir(dbPath);
const serverCertificatePath = dbPath + "/server_test.pem";

var ocsp_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: serverCertificatePath,
    tlsCAFile: OCSP_CA_PEM,
};

// Clear the OCSP cache from any previous runs
clearOCSPCache();

// Start with the OCSP-enabled server certificate
copyCertificateFile(OCSP_SERVER_CERT, serverCertificatePath);

var mongod = null;

assert.doesNotThrow(() => {
    // Start the Mongo server without the mock OCSP server, but with ocspEnabled=true.
    // The server uses a certificate with the following X509v3 extension:
    // Authority Information Access:
    // OCSP -
    //    URI: http:  // localhost:8100/status
    // We expect the server to continue working as usual and should not crash
    mongod = MongoRunner.runMongod(ocsp_options);
});

// Insert some data
const dbName = jsTestName();
const collName = jsTestName();
const testDB = mongod.getDB(dbName);
assertCreateCollection(testDB, collName);
const coll = testDB.getCollection(collName);
assert.commandWorked(coll.insert({"_id": 1, "title": "employee"}));

// Rotate to a certificate without OCSP
copyCertificateFile(OCSP_NO_OCSP_SERVER_CERT, serverCertificatePath);

assert.doesNotThrow(() => {
    const success = mongod.adminCommand({rotateCertificates: 1}).ok;
});

// Try inserting more data to ensure mongod continues to work with the new
// certificate
assert.commandWorked(coll.insert({"_id": 2, "title": "contractor"}));

MongoRunner.stopMongod(mongod);

clearOCSPCache();
