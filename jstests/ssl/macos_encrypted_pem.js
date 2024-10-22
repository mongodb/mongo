// Test macOS refusing to start up with encrypted PEM file.

import {requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

requireSSLProvider('apple', function() {
    jsTest.log("Verifying that mongod will fail to start using an encrypted PEM file");

    const config = MongoRunner.mongodOptions({
        tlsCertificateKeyFile: "jstests/libs/password_protected.pem",
        tlsMode: "requireTLS",
        tlsCertificateKeyFilePassword: "qwerty",
        tlsCAFile: "jstests/libs/ca.pem",
    });

    assert.throws(() => MongoRunner.runMongod(config), [], "MongoD unexpectedly started up");

    assert.eq(rawMongoProgramOutput(".*").includes(
                  "Using encrypted PKCS#1/PKCS#8 PEM files is not supported on this platform"),
              true);
});
