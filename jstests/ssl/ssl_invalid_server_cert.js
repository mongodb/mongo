// Test invalid SSL keyfile settings.

(function() {
'use strict';

function runTest(name, config, expect) {
    jsTest.log('Running test: ' + name);
    clearRawMongoProgramOutput();

    const mongod = MongoRunner.runMongod(config);
    assert.eq(null, mongod, 'Mongod started unexpectedly');

    const output = rawMongoProgramOutput();
    assert.eq(
        true, output.includes(expect), "Server failure message did not include '" + expect + "'");
}

const validityMessage = 'The provided SSL certificate is expired or not yet valid';

// Test that startup fails with certificate that has yet to become valid.
const notYetValid = {
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: 'jstests/libs/not_yet_valid.pem',
    tlsCAFile: 'jstests/libs/ca.pem',
};
runTest('not-yet-valid', notYetValid, validityMessage);

// Test that startup fails with expired certificate.
const expired = {
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: 'jstests/libs/expired.pem',
    tlsCAFile: 'jstests/libs/ca.pem',
};
runTest('expired', expired, validityMessage);

// Test that startup fails with no certificate at all.
const needKeyFile = 'need tlsCertificateKeyFile or certificateSelector when TLS is enabled';
runTest('no-key-file', {tlsMode: 'requireTLS'}, needKeyFile);
})();
