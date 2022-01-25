/**
 * Validate that the shell can load certificates from the certificate store and connect to the
 * server.
 */

load('jstests/ssl/libs/ssl_helpers.js');

requireSSLProvider('windows', function() {
    'use strict';

    if (_isWindows()) {
        // SChannel backed follows Windows rules and only trusts Root in LocalMachine
        runProgram("certutil.exe", "-addstore", "-f", "Root", "jstests\\libs\\trusted-ca.pem");

        // Import a pfx file since it contains both a cert and private key and is easy to import
        // via command line.
        runProgram("certutil.exe",
                   "-importpfx",
                   "-f",
                   "-p",
                   "qwerty",
                   "jstests\\libs\\trusted-client.pfx");
    }

    const conn = MongoRunner.runMongod(
        {sslMode: 'requireSSL', sslPEMKeyFile: "jstests\\libs\\trusted-server.pem"});

    const testWithCert = function(certSelector) {
        jsTest.log(`Testing with SSL cert ${certSelector}`);
        const argv = [
            'mongo',
            '--ssl',
            '--sslCertificateSelector',
            certSelector,
            '--port',
            conn.port,
            '--eval',
            'db.runCommand({buildInfo: 1})'
        ];

        const exitStatus = runMongoProgram.apply(null, argv);
        assert.eq(exitStatus, 0, "successfully connected with SSL");
    };

    const trusted_client_thumbprint = cat('jstests/libs/trusted-client.pem.digest.sha1');

    assert.doesNotThrow(function() {
        testWithCert("thumbprint=" + trusted_client_thumbprint);
    });

    assert.doesNotThrow(function() {
        testWithCert("subject=Trusted Kernel Test Client");
    });

    MongoRunner.stopMongod(conn);
});
