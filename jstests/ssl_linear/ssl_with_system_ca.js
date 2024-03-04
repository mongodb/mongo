// On OSX this test assumes that jstests/libs/trusted-ca.pem has been added as a trusted
// certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildslave-cookbooks/commit/af7cabe5b6e0885902ebd4902f7f974b64cc8961
// for details.
// To install trusted-ca.pem for local testing on OSX, invoke the following at a console:
//   security add-trusted-cert -d jstests/libs/trusted-ca.pem

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
jsTest.log("HOST_TYPE = " + HOST_TYPE);

if (HOST_TYPE == "windows") {
    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", "jstests\\libs\\trusted-ca.pem");

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", "jstests\\libs\\trusted-ca.pem");
}
function testWithCerts(prefix) {
    jsTest.log("Starting mongod blindly...");
    // allowTLS to get a non-TLS control connection.
    var opts = {
        tlsMode: 'preferTLS',
        tlsCertificateKeyFile: 'jstests/libs/' + prefix + 'server.pem',
        waitForConnect: false,
        setParameter: {tlsUseSystemCA: true},
        env: {"SSL_CERT_FILE": "jstests/libs/trusted-ca.pem"},
    };
    const conn = MongoRunner.runMongod(opts);

    jsTest.log("Waiting for mongod to be non-TLS connectable...");
    let argv = ['mongo', '--port', conn.port, '--eval', ';'];

    assert.soon((exitCode) => {
        exitCode = runMongoProgram.apply(null, argv);
        return 0 == exitCode;
    });

    jsTest.log("Testing connection with " + prefix + "client.pem ...");
    argv = [
        'mongo',
        '--tls',
        '--port',
        conn.port,
        '--tlsCertificateKeyFile',
        'jstests/libs/' + prefix + 'client.pem',
        '--eval',
        ';'
    ];

    if (HOST_TYPE == "linux") {
        // On Linux we override the default path to the system CA store to point to our
        // "trusted" CA. On Windows, this CA will have been added to the user's trusted CA list
        argv.unshift("env", "SSL_CERT_FILE=jstests/libs/trusted-ca.pem");
    }

    let exitCode = runMongoProgram.apply(null, argv);

    jsTest.log("Stopping mongod...");
    MongoRunner.stopMongod(conn);

    return exitCode;
}

try {
    assert.neq(0, testWithCerts(''), 'Certs signed with untrusted CA');
    assert.eq(0, testWithCerts('trusted-'), 'Certs signed with trusted CA');
} finally {
    if (HOST_TYPE == "windows") {
        const trusted_ca_thumbprint = cat('jstests/libs/trusted-ca.pem.digest.sha1');
        runProgram("certutil.exe", "-delstore", "-f", "Root", trusted_ca_thumbprint);
        runProgram("certutil.exe", "-delstore", "-user", "-f", "CA", trusted_ca_thumbprint);
    }
}
