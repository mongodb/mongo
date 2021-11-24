// Verifies that if the server obtains a no-nextUpdate OCSP response,
// that it doesn't staple, and refetches after the backoff timeout.
// @tags: [requires_http_client, requires_ocsp_stapling]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (!supportsStapling()) {
    return;
}

// Setting the seconds to 0 in the mock responder will cause it to omit
// the nextUpdate field in the response.
const RESPONSE_VALIDITY = 0;  // seconds

let mock_ocsp = new MockOCSPServer("", RESPONSE_VALIDITY);
let conn = null;
mock_ocsp.start();

const ocsp_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT,
    sslCAFile: OCSP_CA_PEM,
    sslAllowInvalidHostnames: "",
    setParameter: {
        "ocspEnabled": "true",
    },
};

// ====== TEST 1
jsTestLog("Test server refetches cert status if the response has no nextUpdate");

conn = MongoRunner.runMongod(ocsp_options);
sleep(10000);

// validate that fetchAndStaple was invoked at least 5 times in the 10+ seconds
// since the mongod process started.
const FETCH_LOG_ID = 6144500;
assert.eq(true,
          checkLog.checkContainsWithAtLeastCountJson(conn, FETCH_LOG_ID, {}, 5),
          'Number of log lines with ID ' + FETCH_LOG_ID + ' is less than expected');

MongoRunner.stopMongod(conn);

// ====== TEST 2
jsTestLog("Test server is not stapling the response");

ocsp_options.sslPEMKeyFile = OCSP_SERVER_MUSTSTAPLE_CERT;
ocsp_options.waitForConnect = false;

conn = MongoRunner.runMongod(ocsp_options);
waitForServer(conn);
// wait for server to do several rounds of OCSP fetch
sleep(5000);

// client connection is expected to fail because of the muststaple server cert
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
