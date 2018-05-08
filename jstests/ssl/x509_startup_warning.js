// Test for startuo warning when X509 auth and sslAllowInvalidCertificates are enabled

(function() {
    'use strict';

    function runTest(opts, expectWarning) {
        clearRawMongoProgramOutput();
        const mongod = MongoRunner.runMongod(Object.assign({
            auth: '',
            sslMode: 'requireSSL',
            sslPEMKeyFile: 'jstests/libs/server.pem',
            sslCAFile: 'jstests/libs/ca.pem',
        },
                                                           opts));
        assert.eq(expectWarning,
                  rawMongoProgramOutput().includes(
                      'WARNING: While invalid X509 certificates may be used'));
        MongoRunner.stopMongod(mongod);
    }

    // Don't expect a warning when we're not using both options together.
    runTest({}, false);
    runTest({sslAllowInvalidCertificates: '', setParameter: 'authenticationMechanisms=SCRAM-SHA-1'},
            false);
    runTest({setParameter: 'authenticationMechanisms=MONGODB-X509'}, false);
    runTest({clusterAuthMode: 'x509'}, false);

    // Do expect a warning when we're combining options.
    runTest(
        {sslAllowInvalidCertificates: '', setParameter: 'authenticationMechanisms=MONGODB-X509'},
        true);
    runTest({sslAllowInvalidCertificates: '', clusterAuthMode: 'x509'}, true);
})();
