// Check that OCSP verification works
// @tags: [requires_http_client, requires_ocsp_stapling]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (!supportsStapling()) {
    return;
}

let mock_ocsp = new MockOCSPServer("", 20);
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

let conn = null;

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

mock_ocsp.stop();
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1000);
mock_ocsp.start();

// We're sleeping here to give the server enough time to fetch a new OCSP response
// saying that it's revoked.
sleep(15000);

assertClientConnectFails(conn, OCSP_REVOKED);

mock_ocsp.stop();
mock_ocsp = new MockOCSPServer("", 1000);
mock_ocsp.start();

// This ensures that the client was viewing a stapled response.
assertClientConnectFails(conn, OCSP_REVOKED);

MongoRunner.stopMongod(conn);

// have the server refresh its response every 10 seconds
Object.extend(ocsp_options,
              {setParameter: {ocspEnabled: true, ocspValidationRefreshPeriodSecs: 10}});
assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

// Have the OCSP server revoke the certificate. Clients should observe a refreshed stapled response
// after ocspValidationRefreshPeriodSecs
mock_ocsp.stop();
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 10);
mock_ocsp.start();
sleep(30000);
// the client should be trying to connect after its certificate has been revoked.
assertClientConnectFails(conn, OCSP_REVOKED);
MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();

// Next, we're going to test that the server deletes its OCSP response before the
// response expires.
const NEXT_UPDATE = 10;

mock_ocsp = new MockOCSPServer("", NEXT_UPDATE);
mock_ocsp.start();

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

mock_ocsp.stop();

// If the server stapled an expired response, then the client would refuse to connect.
// We now check that the server has not stapled a response.
sleep(NEXT_UPDATE * 1000);
assertClientConnectSucceeds(conn);

MongoRunner.stopMongod(conn);

// Make sure that the refresh period is set to a very large value so that we can
// make sure that the period defined by the mock OCSP responder overrides it.
let ocsp_options_high_refresh = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT,
    sslCAFile: OCSP_CA_PEM,
    sslAllowInvalidHostnames: "",
    setParameter: {
        "ocspEnabled": "true",
        "ocspStaplingRefreshPeriodSecs": 300000,
    },
};

mock_ocsp = new MockOCSPServer("", 10);
mock_ocsp.start();

assert.doesNotThrow(() => {
    conn = MongoRunner.runMongod(ocsp_options);
});

// Kill the ocsp responder, start it with cert revoked, and wait 20 seconds
// so the server refreshes its stapled OCSP response.
mock_ocsp.stop();
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 100);
mock_ocsp.start();

sleep(20000);

// By asserting here that a new connection cannot be established to the
// mongod, we prove that the server has refreshed its stapled response sooner
// than the refresh period indicated.
assertClientConnectFails(conn, OCSP_REVOKED);

MongoRunner.stopMongod(conn);

// The mongoRunner spawns a new Mongo Object to validate the collections which races
// with the shutdown logic of the mock_ocsp responder on some platforms. We need this
// sleep to make sure that the threads don't interfere with each other.
sleep(1000);
mock_ocsp.stop();
}());
