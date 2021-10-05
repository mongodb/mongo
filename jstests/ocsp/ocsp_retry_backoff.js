// Check that OCSP retry backoffs work
// @tags: [requires_http_client, requires_ocsp_stapling]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (!supportsStapling()) {
    return;
}

const RESPONSE_VALIDITY = 5;  // seconds

const mock_ocsp = new MockOCSPServer("", RESPONSE_VALIDITY);

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
jsTestLog("Test retry backoffs when no OCSP responder available on startup");

let conn = MongoRunner.runMongod(ocsp_options);

sleep(10000);

// validate that fetchAndStaple was invoked at least 5 times in the 10+ seconds
// since the mongod process started.
const FETCH_LOG_ID = 577164;
assert.eq(true,
          checkLog.checkContainsWithAtLeastCountJson(conn, FETCH_LOG_ID, {}, 5),
          'Number of log lines with ID ' + FETCH_LOG_ID + ' is less than expected');

MongoRunner.stopMongod(conn);

// ====== TEST 2
jsTestLog("Test fetcher can recover on transient outages of the mock responder");
ocsp_options.sslPEMKeyFile = OCSP_SERVER_MUSTSTAPLE_CERT;
ocsp_options.waitForConnect = false;

conn = MongoRunner.runMongod(ocsp_options);
waitForServer(conn);

assert.throws(() => {
    new Mongo(conn.host);
});

// give mongod some time to re-fetch
mock_ocsp.start();
sleep(10000);

assert.doesNotThrow(() => {
    new Mongo(conn.host);
});

// take down the responder and let the cached response expire
mock_ocsp.stop();
sleep(RESPONSE_VALIDITY * 1000);

assert.throws(() => {
    new Mongo(conn.host);
});

// restart the responder & verify mongod can soon fetch successfully
mock_ocsp.start();
sleep(10000);

assert.doesNotThrow(() => {
    new Mongo(conn.host);
});

MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();
}());
