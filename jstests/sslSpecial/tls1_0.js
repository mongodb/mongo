// Make sure MongoD starts with TLS 1.0 disabled (except w/ old OpenSSL).

(function() {
    'use strict';

    load("jstests/ssl/libs/ssl_helpers.js");

    // There will be cases where a connect is impossible,
    // let the test runner clean those up.
    TestData.failIfUnterminatedProcesses = false;

    const supportsTLS1_1 = (function() {
        const openssl = getBuildInfo().openssl || {};
        if (openssl.compiled === undefined) {
            // Native TLS build.
            return true;
        }
        // OpenSSL 0.x.x => TLS 1.0 only.
        if (/OpenSSL 0\./.test(openssl.compiled)) {
            return false;
        }
        // OpenSSL 1.0.0-1.0.0k => TLS 1.0 only.
        if (/OpenSSL 1\.0\.0[ a-k]/.test(openssl.compiled)) {
            return false;
        }

        // OpenSSL 1.0.0l and later include TLS 1.1 and 1.2
        return true;
    })();

    const defaultEnableTLS1_0 = (function() {
        // If the build doesn't support TLS 1.1, then TLS 1.0 is left enabled.
        return !supportsTLS1_1;
    })();

    const supportsTLS1_3 = detectDefaultTLSProtocol() !== "TLS1_2";

    function test(serverDP, clientDP, shouldSucceed) {
        const expectLogMessage = !defaultEnableTLS1_0 && (serverDP === null);
        let serverOpts = {
            sslMode: 'allowSSL',
            sslPEMKeyFile: 'jstests/libs/server.pem',
            sslCAFile: 'jstests/libs/ca.pem',
            waitForConnect: true
        };
        if (serverDP !== null) {
            serverOpts.sslDisabledProtocols = serverDP;
        }
        clearRawMongoProgramOutput();
        const mongod = MongoRunner.runMongod(serverOpts);
        if (!mongod) {
            assert(!shouldSucceed);
            return;
        }

        let clientOpts = [];
        if (clientDP !== null) {
            clientOpts = ['--sslDisabledProtocols', clientDP];
        }
        const didSucceed = (0 == runMongoProgram('mongo',
                                                 '--ssl',
                                                 '--port',
                                                 mongod.port,
                                                 '--sslPEMKeyFile',
                                                 'jstests/libs/client.pem',
                                                 '--sslCAFile',
                                                 'jstests/libs/ca.pem',
                                                 ...clientOpts,
                                                 '--eval',
                                                 ';'));

        MongoRunner.stopMongod(mongod);

        // Exit code based success/failure.
        assert.eq(
            didSucceed, shouldSucceed, "Running with " + tojson(serverDP) + "/" + tojson(clientDP));

        assert.eq(expectLogMessage,
                  rawMongoProgramOutput().search('Automatically disabling TLS 1.0') >= 0,
                  "TLS 1.0 was/wasn't automatically disabled");
    }

    // Tests with default client behavior (TLS 1.0 disabled if 1.1 available).
    test(null, null, true);
    test('none', null, true);
    test('TLS1_0', null, supportsTLS1_1);
    test('TLS1_1,TLS1_2', null, !supportsTLS1_1 || supportsTLS1_3);
    test('TLS1_1,TLS1_2,TLS1_3', null, !supportsTLS1_1);
    test('TLS1_0,TLS1_1', null, supportsTLS1_1);
    test('TLS1_0,TLS1_1,TLS1_2', null, supportsTLS1_3);
    test('TLS1_0,TLS1_1,TLS1_2,TLS1_3', null, false);

    // Tests with TLS 1.0 always enabled on client.
    test(null, 'none', true);
    test('none', 'none', true);
    test('TLS1_0', 'none', supportsTLS1_1);
    test('TLS1_1,TLS1_2', 'none', true);
    test('TLS1_0,TLS1_1', 'none', supportsTLS1_1);

    // Tests with TLS 1.0 explicitly disabled on client.
    test(null, 'TLS1_0', supportsTLS1_1);
    test('none', 'TLS1_0', supportsTLS1_1);
    test('TLS1_0', 'TLS1_0', supportsTLS1_1);
    test('TLS1_1,TLS1_2', 'TLS1_0', supportsTLS1_3);
    test('TLS1_1,TLS1_2,TLS1_3', 'TLS1_0', false);
    test('TLS1_0,TLS1_1', 'TLS1_0', supportsTLS1_1);
})();
