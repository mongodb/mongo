// Check that OCSP stapling works
// @tags: [requires_http_client, requires_ocsp_stapling]

import {
    FAULT_REVOKED,
    MockOCSPServer,
    OCSP_CA_RESPONDER,
    OCSP_DELEGATE_RESPONDER,
    OCSP_INTERMEDIATE_RESPONDER,
} from "jstests/ocsp/lib/mock_ocsp.js";
import {
    CLUSTER_CA_CERT,
    CLUSTER_KEY,
    OCSP_CA_PEM,
    OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
    OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM,
    OCSP_SERVER_CERT,
    OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
    supportsStapling,
    waitForServer
} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
}

const CLUSTER_CA = {
    tlsClusterFile: CLUSTER_KEY,
    tlsClusterCAFile: CLUSTER_CA_CERT,
    tlsAllowConnectionsWithoutCertificates: "",
    tlsAllowInvalidCertificates: "",
};

function test(serverCert, caCert, responderCertPair, extraOpts) {
    const ocsp_options = {
        sslMode: "requireSSL",
        sslPEMKeyFile: serverCert,
        sslCAFile: caCert,
        sslAllowInvalidHostnames: "",
        setParameter: {
            "ocspStaplingRefreshPeriodSecs": 500,
            "ocspEnabled": "true",
        }
    };

    if (extraOpts) {
        Object.extend(ocsp_options, extraOpts);
    }

    // This is to test what happens when the responder is down,
    // making sure that we soft fail.
    let conn = null;

    assert.doesNotThrow(() => {
        conn = MongoRunner.runMongod(ocsp_options);
    });

    MongoRunner.stopMongod(conn);

    let mock_ocsp = new MockOCSPServer("", 1000, responderCertPair);
    mock_ocsp.start();

    // In this scenario, the Mongod has the ocsp response stapled
    // which should allow the connection to proceed. Even when the
    // responder says that the certificate is revoked, the mongod
    // should still have the old response stashed and doesn't have
    // to refresh the response, so the shell should connect.
    assert.doesNotThrow(() => {
        conn = MongoRunner.runMongod(ocsp_options);
    });
    mock_ocsp.stop();

    mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1000, responderCertPair);
    mock_ocsp.start();
    assert.doesNotThrow(() => {
        new Mongo(conn.host);
    });

    MongoRunner.stopMongod(conn);

    // This is the same scenario as above, except that the mongod has
    // the status saying that the certificate is revoked. If we have a shell
    // waiting to connect, it will fail because the certificate status of
    // the mongod's cert is revoked.
    Object.extend(ocsp_options, {waitForConnect: false});
    conn = MongoRunner.runMongod(ocsp_options);

    waitForServer(conn);

    assert.throws(() => {
        new Mongo(conn.host);
    });
    mock_ocsp.stop();

    mock_ocsp = new MockOCSPServer("", 1000, responderCertPair);
    mock_ocsp.start();

    assert.throws(() => {
        new Mongo(conn.host);
    });

    MongoRunner.stopMongod(conn);

    // The mongoRunner spawns a new Mongo Object to validate the collections which races
    // with the shutdown logic of the mock_ocsp responder on some platforms. We need this
    // sleep to make sure that the threads don't interfere with each other.
    sleep(1000);
    mock_ocsp.stop();
}

function testSuperLongOCSPResponseNextUpdateTime() {
    const ocsp_options = {
        sslMode: "requireSSL",
        sslPEMKeyFile: OCSP_SERVER_CERT,
        sslCAFile: OCSP_CA_PEM,
        sslAllowInvalidHostnames: "",
        setParameter: {
            "ocspEnabled": "true",
        }
    };

    let conn = null;

    // Converting this to nanoseconds would overflow a 64-bit long long
    const kSuperLongNextUpdateSeconds = 20000000000;
    const mock_ocsp = new MockOCSPServer("", kSuperLongNextUpdateSeconds);
    mock_ocsp.start();
    assert.doesNotThrow(() => {
        conn = MongoRunner.runMongod(ocsp_options);
    });

    mock_ocsp.stop();
    MongoRunner.stopMongod(conn);
}

test(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_DELEGATE_RESPONDER);
test(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_DELEGATE_RESPONDER, CLUSTER_CA);
test(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_CA_RESPONDER);
test(OCSP_SERVER_CERT, OCSP_CA_PEM, OCSP_CA_RESPONDER, CLUSTER_CA);

// This test can not be repeated with CLUSTER_CA, because intermediate cert
// is not part of cluster CA chain
test(OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
     OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM,
     OCSP_INTERMEDIATE_RESPONDER);

test(OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM, OCSP_CA_PEM, OCSP_INTERMEDIATE_RESPONDER);
test(OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM,
     OCSP_CA_PEM,
     OCSP_INTERMEDIATE_RESPONDER,
     CLUSTER_CA);

testSuperLongOCSPResponseNextUpdateTime();
