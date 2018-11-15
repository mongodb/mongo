// Test that the server supports at least one ECDHE tls cipher suite.

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
        tlsPEMKeyFile: SERVER_CERT,
        ipv6: "",
        bind_ip_all: ""
    };

    const mongod = MongoRunner.runMongod(x509_options);

    // Run the tls cipher suite enumerator
    let python = "/usr/bin/env /opt/mongodbtoolchain/v2/bin/python3";
    let enumerator = " jstests/ssl/tls_enumerator.py ";
    const python_command = python + enumerator + "--port=" + mongod.port + " --cafile=" + CA_CERT +
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

    // Printing TLS 1.1 and 1.2 suites that are accepted
    let hasECDHE = false;
    suites.slice(suites.indexOf('tls1_1')).forEach(tlsVersion => {
        print('*************************\n' + tlsVersion + ": ");
        cipherDict[tlsVersion].forEach(cipher => {
            print(cipher);
            if (cipher.includes('ECDHE'))
                hasECDHE = true;
        });
    });

    // All platforms except Amazon Linux 1 should support ECDHE
    if (!EXCLUDED_BUILDS.includes(buildInfo().buildEnvironment.distmod)) {
        assert(hasECDHE, 'Supports at least one ECDHE cipher suite');
    }

    MongoRunner.stopMongod(mongod);
}());
