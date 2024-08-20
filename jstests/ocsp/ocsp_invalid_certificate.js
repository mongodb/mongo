// Check that OCSP verification may be used with invalid certificates
// @tags: [requires_http_client, requires_ocsp_stapling]

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    FAULT_REVOKED,
    MockOCSPServer,
    OCSP_DELEGATE_RESPONDER
} from "jstests/ocsp/lib/mock_ocsp.js";
import {
    OCSP_CA_PEM,
    OCSP_SERVER_CERT_INVALID,
    supportsStapling
} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
}

/**
 * We wish to test a potential race condition where a node opens an egress connection,
 *  kicks off an OCSP verification task, discovers that the certificate is invalid and terminates
 * the connection, and then asynchonously concludes the verification task. We want to set up a
 * scenario with all of the following:
 *   - We need a two node cluster in order to trigger the egress connection.
 *   - Neither node should have stapled responses.
 *   - Both nodes must possess invalid certificates.
 *   - Neither node should enable tlsAllowInvalidHostnames.
 */

// Delay OCSP responses by 3 seconds.
// We'll ensure that nodes wait only 1 second to staple responses.
// This ensures that nodes will refuse to staple.
// We'll limit the next_update to 1 second, to force frequent cache invalidation.
let mock_ocsp = new MockOCSPServer("", 1, OCSP_DELEGATE_RESPONDER, 3);
mock_ocsp.start();

// Set Default timeout time to 2 minutes so test doesn't
// run forever, in successful cases.
ReplSetTest.kDefaultTimeoutMS = 2 * 60 * 1000;

// We don't want to invoke the hang analyzer because we
// expect this test to fail by timing out
MongoRunner.runHangAnalyzer.disable();

const ocsp_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT_INVALID,
    sslCAFile: OCSP_CA_PEM,
    setParameter: {
        "ocspEnabled": "true",
        "tlsOCSPStaplingTimeoutSecs": 1,
    },
};

const rstest = new ReplSetTest({
    name: "OCSP Servers Test",
    nodes: 2,
    nodeOptions: ocsp_options,
});

// Spawn the replicaset. This operation is expected to fail, because the nodes will reject each
// others' certificates.
try {
    rstest.startSet();
    rstest.initiate();
} catch (e) {
}

// Though the replicaset is unhealthy, assert that all of the nodes are still be alive
rstest.nodes.forEach((node) => {
    assert.commandWorked(node.getDB("admin").runCommand({ping: 1}));
});

// Finally, try to shutdown the set, but accept that it might not be healthy enough to do so.
try {
    rstest.stopSet(15);
} catch (e) {
}
mock_ocsp.stop();
