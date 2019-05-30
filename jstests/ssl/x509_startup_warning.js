// Test for startuo warning when X509 auth and sslAllowInvalidCertificates are enabled

(function() {
    'use strict';

    function runTest(checkMongos, opts, expectWarningCertifcates, expectWarningHostnames) {
        clearRawMongoProgramOutput();
        let mongo;

        if (checkMongos) {
            mongo = MongoRunner.runMongos(Object.assign({
                configdb: "fakeRS/localhost:27017",
                waitForConnect: false,
            },
                                                        opts));
        } else {
            mongo = MongoRunner.runMongod(Object.assign({
                auth: '',
                sslMode: 'preferSSL',
                sslPEMKeyFile: 'jstests/libs/server.pem',
                sslCAFile: 'jstests/libs/ca.pem',
                waitForConnect: false,
            },
                                                        opts));
        }

        assert.soon(function() {
            const output = rawMongoProgramOutput();
            return (expectWarningCertifcates ==
                        output.includes('WARNING: While invalid X509 certificates may be used') &&
                    expectWarningHostnames ==
                        output.includes(
                            'WARNING: This server will not perform X.509 hostname validation'));
        });

        stopMongoProgramByPid(mongo.pid);
    }

    function runTests(checkMongos) {
        // Don't expect a warning for certificates and hostnames when we're not using both options
        // together.
        runTest(checkMongos, {}, false, false);

        // Do expect a warning for certificates when we're combining options.
        runTest(checkMongos, {sslAllowInvalidCertificates: ''}, true, false);

        // Do expect a warning for hostnames.
        runTest(checkMongos, {sslAllowInvalidHostnames: ''}, false, true);

        // Do expect a warning for certificates and hostnames.
        runTest(checkMongos,
                {sslAllowInvalidCertificates: '', sslAllowInvalidHostnames: ''},
                true,
                true);
    }

    // Run tests on mongos
    runTests(true);

    // Run tests on mongod
    runTests(false);

})();
