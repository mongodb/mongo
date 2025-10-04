// Check that OCSP verification works
// @tags: [requires_http_client]

import {MockOCSPServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {clearOCSPCache, OCSP_CA_PEM, OCSP_SERVER_CERT} from "jstests/ocsp/lib/ocsp_helpers.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() !== "windows") {
    quit();
}

let ocsp_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: OCSP_SERVER_CERT,
    tlsCAFile: OCSP_CA_PEM,
    tlsAllowInvalidHostnames: "",
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

let conn = null;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

const WARN_ID = 4780400;
assert.eq(true, checkLog.checkContainsOnceJson(conn, WARN_ID, {}), "Expected log ID " + WARN_ID + " was not found");

MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();
