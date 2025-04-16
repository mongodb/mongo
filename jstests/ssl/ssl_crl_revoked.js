// Test a revoked CRL -- ensure that a connection is not allowed.
// Note: crl_client_revoked.pem is a CRL with the client.pem certificate listed as revoked.
// This test should test that the user cannot connect with client.pem certificate.

import {requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

requireSSLProvider(['openssl', 'windows'], function() {
    var md = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: "jstests/libs/server.pem",
        tlsCAFile: "jstests/libs/ca.pem",
        tlsCRLFile: "jstests/libs/crl_client_revoked.pem"
    });

    var mongo = runMongoProgram("mongo",
                                "--port",
                                md.port,
                                "--tls",
                                "--tlsCAFile",
                                "jstests/libs/ca.pem",
                                "--tlsCertificateKeyFile",
                                "jstests/libs/client_revoked.pem",
                                "--eval",
                                ";");

    // 1 is the exit code for the shell failing to connect, which is what we want
    // for a successful test.
    assert(mongo == 1);
    MongoRunner.stopMongod(md);
});
