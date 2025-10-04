// Check that OCSP verification works
// @tags: [requires_http_client, requires_ocsp_stapling]

import {MockOCSPServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {
    OCSP_CA_PEM,
    OCSP_SERVER_MUSTSTAPLE_CERT,
    supportsStapling,
    waitForServer,
} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
}

let mock_ocsp = new MockOCSPServer();
mock_ocsp.start();

let ocsp_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: OCSP_SERVER_MUSTSTAPLE_CERT,
    tlsCAFile: OCSP_CA_PEM,
    tlsAllowInvalidHostnames: "",
    setParameter: {
        "ocspEnabled": "true",
    },
};

jsTestLog("Testing regular stapling with a server using a MustStaple certificate.");
let conn = MongoRunner.runMongod(ocsp_options);

// this connection should succeed, since the server will staple responses
new Mongo(conn.host);

MongoRunner.stopMongod(conn);

ocsp_options = Object.merge(ocsp_options, {
    setParameter: {ocspEnabled: true, "failpoint.disableStapling": "{mode: 'alwaysOn'}"},
    waitForConnect: false,
});

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});
jsTestLog(
    "Testing that a client can connect to a server using a MustStaple certificate and tlsAllowInvalidCertificates enabled.",
);
waitForServer(conn);

// assert that trying to connect to a server using a MustStaple certificate without a stapled OCSP
// response will fail
jsTestLog(
    "Testing that a client cannot connect to a server using a MustStaple certificate without a stapled response.",
);
assert.throws(() => {
    new Mongo(conn.host);
});

MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();
