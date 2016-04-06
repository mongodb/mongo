((function() {
    'use strict';
    const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;

    if (HOST_TYPE == "windows") {
        runProgram(
            "certutil.exe", "-addstore", "-user", "-f", "CA", "jstests\\libs\\trusted-ca.pem");
    } else if (HOST_TYPE == "osx") {
        runProgram(
            "/bin/sh",
            "-c",
            "/usr/bin/security add-trusted-cert -k ~/Library/Keychains/login.keychain jstests/libs/trusted-ca.pem");
    }

    var testWithCerts = function(serverPem) {
        jsTest.log(`Testing with SSL certs $ {
            serverPem
        }`);
        // allowSSL instead of requireSSL so that the non-SSL connection succeeds.
        var conn = MongoRunner.runMongod(
            {sslMode: 'requireSSL', sslPEMKeyFile: "jstests/libs/" + serverPem});

        // Should not be able to authenticate with x509.
        // Authenticate call will return 1 on success, 0 on error.
        var argv =
            ['./mongo', '--ssl', '--port', conn.port, '--eval', ('db.runCommand({buildInfo: 1})')];
        if (HOST_TYPE == "linux") {
            // On Linux we override the default path to the system CA store to point to our
            // "trusted" CA. On Windows, this CA will have been added to the user's trusted CA list
            argv.unshift("env", "SSL_CERT_FILE=jstests/libs/trusted-ca.pem");
        }
        var exitStatus = runMongoProgram.apply(null, argv);
        assert.eq(exitStatus, 0, "successfully connected with SSL");

        MongoRunner.stopMongod(conn.port);
    };

    assert.throws(function() {
        testWithCerts("server.pem", "client.pem");
    });
    assert.doesNotThrow(function() {
        testWithCerts("trusted-server.pem", "trusted-client.pem");
    });
})());
