/**
 * Validate that the shell can load certificates from the certificate store and connect to the
 * server.
 */

import {getPython3Binary} from "jstests/libs/python.js";
import {
    requireSSLProvider,
    TRUSTED_CA_CERT,
    TRUSTED_SERVER_CERT
} from "jstests/ssl/libs/ssl_helpers.js";

requireSSLProvider('windows', function() {
    if (_isWindows()) {
        assert.eq(0,
                  runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"));

        // SChannel backed follows Windows rules and only trusts Root in LocalMachine
        runProgram("certutil.exe", "-addstore", "-f", "Root", TRUSTED_CA_CERT);
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
        const mongod = MongoRunner.runMongod({
            tlsMode: 'requireTLS',
            tlsCertificateKeyFile: TRUSTED_SERVER_CERT,
            setParameter: {tlsUseSystemCA: true},
            useHostname: false,
        });

        const testWithCert = function(certSelector) {
            jsTest.log(`Testing with SSL cert ${certSelector}`);
            const conn = new Mongo(mongod.host, undefined, {
                tls: {
                    certificateSelector: certSelector,
                }
            });
            assert.commandWorked(conn.getDB('admin').runCommand({buildinfo: 1}));
        };

        const trusted_client_thumbprint = cat('jstests/libs/trusted-client.pem.digest.sha1');

        assert.doesNotThrow(function() {
            testWithCert("thumbprint=" + trusted_client_thumbprint);
        });

        assert.doesNotThrow(function() {
            testWithCert("subject=Trusted Kernel Test Client");
        });

        MongoRunner.stopMongod(mongod);
    } finally {
        if (_isWindows()) {
            const trusted_ca_thumbprint = cat('jstests/libs/trusted-ca.pem.digest.sha1');
            runProgram("certutil.exe", "-delstore", "-f", "Root", trusted_ca_thumbprint);
        }
    }
});
