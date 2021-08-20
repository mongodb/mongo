// Check that OCSP verification works
// @tags: [requires_http_client]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (determineSSLProvider() !== "windows") {
    return;
}

var ocsp_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT,
    sslCAFile: OCSP_CA_PEM,
    sslAllowInvalidHostnames: "",
    setParameter: {
        "failpoint.disableStapling": "{'mode':'alwaysOn'}",
        "ocspEnabled": "true",
        "tlsOCSPSlowResponderWarningSecs": 3,
        "tlsOCSPVerifyTimeoutSecs": 10,
    },
};

clearOCSPCache();

let mock_ocsp = new MockOCSPServer("", 1, undefined, 3);
mock_ocsp.start();

var conn = null;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

const WARN_ID = 4780400;
assert.eq(true,
          checkLog.checkContainsOnceJson(conn, WARN_ID, {}),
          'Expected log ID ' + WARN_ID + ' was not found');

MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();
}());
