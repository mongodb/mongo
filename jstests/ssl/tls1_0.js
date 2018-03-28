// Make sure MongoD starts with TLS 1.0 disabled (except w/ old OpenSSL).

(function() {
    'use strict';

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
        if (!supportsTLS1_1) {
            return true;
        }
        // If we're on Apple, then TLS 1.0 is left enabled regardless
        // to support other tools on the system which may be TLS 1.0 only.
        const buildEnv = getBuildInfo().buildEnvironment || {};
        return (buildEnv.target_os === 'macOS');
    })();

    function test(disabledProtocols, shouldSucceed) {
        const expectLogMessage = !defaultEnableTLS1_0 && (disabledProtocols === null);
        let serverOpts = {
            sslMode: 'allowSSL',
            sslPEMKeyFile: 'jstests/libs/server.pem',
            sslCAFile: 'jstests/libs/ca.pem',
            waitForConnect: false
        };
        if (disabledProtocols !== null) {
            serverOpts.sslDisabledProtocols = disabledProtocols;
        }
        clearRawMongoProgramOutput();
        const mongod = MongoRunner.runMongod(serverOpts);
        assert(mongod);

        const didSucceed = (function() {
            try {
                assert.soon(function() {
                    return 0 == runMongoProgram('mongo',
                                                '--ssl',
                                                '--port',
                                                mongod.port,
                                                '--sslPEMKeyFile',
                                                'jstests/libs/client.pem',
                                                '--sslCAFile',
                                                'jstests/libs/ca.pem',
                                                '--eval',
                                                ';');
                }, "Connecting to mongod", 30 * 1000);
                return true;
            } catch (e) {
                return false;
            }
        })();

        // Exit code based success/failure.
        assert.eq(didSucceed,
                  shouldSucceed,
                  "Running with disabledProtocols == " + tojson(disabledProtocols));

        assert.eq(expectLogMessage,
                  rawMongoProgramOutput().search('Automatically disabling TLS 1.0') >= 0,
                  "TLS 1.0 was/wasn't automatically disabled");

        const exitCode =
            (didSucceed || !_isWindows()) ? MongoRunner.EXIT_CLEAN : MongoRunner.EXIT_SIGKILL;
        MongoRunner.stopMongod(mongod, undefined, {allowedExitCode: exitCode});
    }

    test(null, true);
    test('none', true);
    test('TLS1_0', supportsTLS1_1);
    test('TLS1_1,TLS1_2', true);
    test('TLS1_0,TLS1_1', supportsTLS1_1);
    test('TLS1_0,TLS1_1,TLS1_2', false);
})();
