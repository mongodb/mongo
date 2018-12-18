// Test that the server supports ECDHE and DHE tls cipher suites.

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

    // Need to use toolchain python, which is unsupported on Windows
    if (_isWindows()) {
        return;
    }

    // Amazon linux does not currently support ECDHE
    const EXCLUDED_BUILDS = ['amazon', 'amzn64'];

    const SERVER_CERT = "jstests/libs/server.pem";
    const OUTFILE = 'jstests/ssl/ciphers.json';

    const suites = [
        'sslv2',
        'sslv3',
        'tls1',
        'tls1_1',
        'tls1_2',
    ];

    const x509_options = {
        tlsMode: 'requireTLS',
        tlsCAFile: CA_CERT,
        tlsCertificateKeyFile: SERVER_CERT,
        ipv6: "",
        bind_ip_all: ""
    };

    const mongod = MongoRunner.runMongod(x509_options);

    // Use new toolchain python, if it exists
    let python_binary = '/opt/mongodbtoolchain/v3/bin/python3';
    if (runProgram('/bin/sh', '-c', 'ls ' + python_binary) !== 0) {
        python_binary = '/opt/mongodbtoolchain/v2/bin/python3';
    }

    // Run the tls cipher suite enumerator
    const python = '/usr/bin/env ' + python_binary;
    const enumerator = " jstests/ssl/tls_enumerator.py ";
    const python_command = python + enumerator + '--port=' + mongod.port + ' --cafile=' + CA_CERT +
        ' --cert=' + CLIENT_CERT + ' --outfile=' + OUTFILE;
    assert.eq(runProgram('/bin/sh', '-c', python_command), 0);

    // Parse its output
    let cipherDict = {};
    try {
        cipherDict = JSON.parse(cat(OUTFILE));
    } catch (e) {
        jsTestLog("Failed to parse ciphers.json");
        throw e;
    } finally {
        const delete_command = 'rm ' + OUTFILE;
        assert.eq(runProgram('/bin/sh', '-c', delete_command), 0);
    }

    // Checking that SSLv2, SSLv3 and TLS 1.0 are not accepted
    suites.slice(0, suites.indexOf('tls1'))
        .forEach(tlsVersion => assert(cipherDict[tlsVersion].length === 0));

    let hasECDHE = false;
    let hasDHE = false;

    // Printing TLS 1.1 and 1.2 suites that are accepted
    suites.slice(suites.indexOf('tls1_1')).forEach(tlsVersion => {
        print('*************************\n' + tlsVersion + ": ");
        cipherDict[tlsVersion].forEach(cipher => {
            print(cipher);

            if (cipher.startsWith('ECDHE')) {
                hasECDHE = true;
            }

            if (cipher.startsWith('DHE')) {
                hasDHE = true;
            }
        });
    });

    // All platforms except Amazon Linux 1 should support ECDHE and DHE
    if (!EXCLUDED_BUILDS.includes(buildInfo().buildEnvironment.distmod)) {
        assert(hasECDHE, 'Supports at least one ECDHE cipher suite');

        // Secure Transport disallows DHE, so we don't require it on those platforms
        if (determineSSLProvider() !== 'apple') {
            assert(hasDHE, 'Supports at least one DHE cipher suite');
        }
    }

    // If ECDHE is enabled, DHE should be too (for Java 7 compat)
    if (determineSSLProvider() !== 'apple') {
        assert(hasDHE === hasECDHE, 'Supports both ECDHE and DHE or neither');
    }

    MongoRunner.stopMongod(mongod);
}());
