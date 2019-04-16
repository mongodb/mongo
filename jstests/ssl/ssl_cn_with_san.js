// Test that a certificate with a valid CN, but invalid SAN
// does not permit connection, but provides a useful error.

(function() {
    'use strict';
    load('jstests/ssl/libs/ssl_helpers.js');

    // server-intermediate-ca was signed by ca.pem, not trusted-ca.pem
    const CA = 'jstests/libs/ca.pem';
    const SERVER = 'jstests/ssl/libs/localhost-cn-with-san.pem';

    const mongod = MongoRunner.runMongod({
        sslMode: 'requireSSL',
        sslPEMKeyFile: SERVER,
        sslCAFile: CA,
    });
    assert(mongod);

    // Try with `tlsAllowInvalidHostnames` to look for the warning.
    clearRawMongoProgramOutput();
    const mongo = runMongoProgram('mongo',
                                  '--tls',
                                  '--tlsCAFile',
                                  CA,
                                  'localhost:' + mongod.port,
                                  '--eval',
                                  ';',
                                  '--tlsAllowInvalidHostnames');
    assert.neq(mongo, 0, "Shell connected when it should have failed");
    assert(rawMongoProgramOutput().includes(' would have matched, but was overridden by SAN'),
           'Expected detail warning not seen');

    // On OpenSSL only, start without `tlsAllowInvalidHostnames`
    // Windowds/Mac will bail out too early to show this message.
    if (determineSSLProvider() === 'openssl') {
        clearRawMongoProgramOutput();
        const mongo = runMongoProgram(
            'mongo', '--tls', '--tlsCAFile', CA, 'localhost:' + mongod.port, '--eval', ';');
        assert.neq(mongo, 0, "Shell connected when it should have failed");
        assert(rawMongoProgramOutput().includes(
                   'CN: localhost would have matched, but was overridden by SAN'),
               'Expected detail warning not seen');
    }

    MongoRunner.stopMongod(mongod);
})();
