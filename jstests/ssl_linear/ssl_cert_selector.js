/**
 * Validate that the shell can load certificates from the certificate store and connect to the
 * server.
 */

import {requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

requireSSLProvider('windows', function() {
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

    try {
        const conn = MongoRunner.runMongod({
            tlsMode: 'requireTLS',
            tlsCertificateKeyFile: "jstests\\libs\\trusted-server.pem",
            setParameter: {tlsUseSystemCA: true},
        });

        const testWithCert = function(certSelector) {
            jsTest.log(`Testing with SSL cert ${certSelector}`);
            const argv = [
                'mongo',
                '--ssl',
                '--tlsCertificateSelector',
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
    } finally {
        if (_isWindows()) {
            const trusted_ca_thumbprint = cat('jstests/libs/trusted-ca.pem.digest.sha1');
            runProgram("certutil.exe", "-delstore", "-f", "Root", trusted_ca_thumbprint);
            const ca_thumbprint = cat('jstests/libs/trusted-ca.pem.digest.sha1');
            runProgram("certutil.exe", "-delstore", "-f", "Root", ca_thumbprint);
        }
    }
});
