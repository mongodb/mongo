// On OSX this test assumes that jstests/libs/trusted-ca.pem has been added as a trusted
// certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildslave-cookbooks/commit/af7cabe5b6e0885902ebd4902f7f974b64cc8961
// for details.
((function() {
    'use strict';
    const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;

    if (HOST_TYPE == "windows") {
        runProgram(
            "certutil.exe", "-addstore", "-user", "-f", "CA", "jstests\\libs\\trusted-ca.pem");
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
