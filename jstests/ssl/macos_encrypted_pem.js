// Test macOS refusing to start up with encrypted PEM file.

load('jstests/ssl/libs/ssl_helpers.js');
requireSSLProvider('apple', function() {
    'use strict';

    jsTest.log("Verifying that mongod will fail to start using an encrypted PEM file");

    const config = MongoRunner.mongodOptions({
        sslPEMKeyFile: "jstests/libs/password_protected.pem",
        sslMode: "requireSSL",
        sslPEMKeyPassword: "qwerty",
        sslCAFile: "jstests/libs/ca.pem",
        useLogFiles: true,
    });

    const mongod = MongoRunner.runMongod(config);
    assert(mongod === null, "MongoD unexpectedly started up");

    const logFile = cat(config.logFile);
    assert.eq(logFile.includes(
                  "Using encrypted PKCS#1/PKCS#8 PEM files is not supported on this platform"),
              true,
              logFile);
});
