// Test mongo shell output logs correct messages when not including certificates or using bad
// certificates.
(function() {
'use strict';

const SERVER_CERT = "jstests/libs/server.pem";
const CA_CERT = "jstests/libs/ca.pem";

const BAD_CLIENT_CERT = 'jstests/libs/trusted-client.pem';

function testConnect(outputLog, ...args) {
    const command = ['mongo', '--host', 'localhost', '--port', mongod.port, '--tls', ...args];

    clearRawMongoProgramOutput();
    const clientPID = _startMongoProgram({args: command});

    assert.soon(function() {
        const output = rawMongoProgramOutput();
        if (output.includes(outputLog)) {
            stopMongoProgramByPid(clientPID);
            return true;
        }
        return false;
    });
}

function runTests() {
    // --tlsCertificateKeyFile not specifed when mongod was started with --tlsCAFile or
    // --tlsClusterCAFile.
    testConnect('No SSL certificate provided by peer', '--tlsCAFile', CA_CERT);

    // Certificate not signed by CA_CERT used.
    testConnect('SSL peer certificate validation failed',
                '--tlsCAFile',
                CA_CERT,
                '--tlsCertificateKeyFile',
                BAD_CLIENT_CERT);
}

// Use tlsClusterCAFile
let mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsClusterCAFile: CA_CERT,
});

runTests();

MongoRunner.stopMongod(mongod);

// Use tlsCAFile
mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
});

runTests();

MongoRunner.stopMongod(mongod);
})();
