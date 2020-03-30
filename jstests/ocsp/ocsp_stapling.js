// Check that OCSP verification works
// @tags: [requires_http_client]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (determineSSLProvider() !== "openssl") {
    return;
}

if (!supportsStapling()) {
    return;
}

var test = function(responderCA) {
    const ocsp_options = {
        sslMode: "requireSSL",
        sslPEMKeyFile: OCSP_SERVER_CERT,
        sslCAFile: OCSP_CA_PEM,
        sslAllowInvalidHostnames: "",
        setParameter: {
            "ocspEnabled": "true",
        },
    };

    // This is to test what happens when the responder is down,
    // making sure that we soft fail.
    let conn = null;

    assert.doesNotThrow(() => {
        conn = MongoRunner.runMongod(ocsp_options);
    });

    MongoRunner.stopMongod(conn);

    let mock_ocsp = new MockOCSPServer("", 1000, responderCA);
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

    mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1000, responderCA);
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

    mock_ocsp = new MockOCSPServer("", 1000, responderCA);
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
};

test(false);
test(true);
}());