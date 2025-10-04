// Check that OCSP verification works
// @tags: [requires_http_client, requires_ocsp_stapling]

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {FAULT_REVOKED, MockOCSPServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {OCSP_CA_PEM, OCSP_SERVER_CERT, supportsStapling} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
}

let mock_ocsp = new MockOCSPServer("", 10);
mock_ocsp.start();

// Set Default timeout time to 2 minutes so test doesn't
// run forever.
ReplSetTest.kDefaultTimeoutMS = 1 * 30 * 1000;

// We don't want to invoke the hang analyzer because we
// expect this test to fail by timing out
MongoRunner.runHangAnalyzer.disable();

const ocsp_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: OCSP_SERVER_CERT,
    tlsCAFile: OCSP_CA_PEM,
    tlsAllowInvalidHostnames: "",
    setParameter: {
        "ocspEnabled": "true",
    },
};

const rstest = new ReplSetTest({
    name: "OCSP Servers Test",
    nodes: 2,
    nodeOptions: ocsp_options,
});

rstest.startSet();

mock_ocsp.stop();

mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 10);
mock_ocsp.start();

sleep(10);

assert.throws(() => {
    rstest.initialize();
});

rstest.stopSet();

mock_ocsp.stop();

mock_ocsp = new MockOCSPServer();
mock_ocsp.start();

let conn = null;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});
mock_ocsp.stop();

mock_ocsp = new MockOCSPServer(FAULT_REVOKED);
mock_ocsp.start();

// The OCSP status of the client's cert would be Revoked,
// but because we don't want the Server to check the status
// of the client's cert, we assert that this doesn't throw.
assert.doesNotThrow(() => {
    new Mongo(conn.host);
});

MongoRunner.stopMongod(conn);

sleep(1000);
mock_ocsp.stop();
