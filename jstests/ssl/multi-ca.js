// Test that servers can use multiple root CAs.

(function() {
"use strict";

load('jstests/ssl/libs/ssl_helpers.js');

// "root-and-trusted-ca.pem" contains the combined ca.pem and trusted-ca.pem certs.
// This *should* permit client.pem or trusted-client.pem to connect equally.
const CA_CERT = 'jstests/ssl/x509/root-and-trusted-ca.pem';
const SERVER_CERT = 'jstests/libs/server.pem';

const CLIENT_CA_CERT = 'jstests/libs/ca.pem';
const CLIENT_CERT = 'jstests/libs/client.pem';
const TRUSTED_CLIENT_CERT = 'jstests/libs/trusted-client.pem';

const mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
});

function testConnect(cert) {
    const mongo = runMongoProgram('mongo',
                                  '--host',
                                  'localhost',
                                  '--port',
                                  mongod.port,
                                  '--tls',
                                  '--tlsCAFile',
                                  CLIENT_CA_CERT,
                                  '--tlsCertificateKeyFile',
                                  cert,
                                  '--eval',
                                  ';');

    assert.eq(0, mongo, 'Connection attempt failed using ' + cert);
}

testConnect(CLIENT_CERT);
testConnect(TRUSTED_CLIENT_CERT);

MongoRunner.stopMongod(mongod);
}());
