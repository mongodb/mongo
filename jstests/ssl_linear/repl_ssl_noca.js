// On MacOS this test assumes that certificates exist at
// /opt/x509/macos-trusted-[ca|server|client].pem, and that /opt/x509/macos-trusted-ca.pem has
// been added as a trusted certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildhost-configuration/blob/1c1fcb51924cd4f1bc9eaf5db23f6e4365d6ba17/roles/macos/tasks/keychains.yml#L58-L87
// for details.
// To install certificates for local testing on OSX, invoke the following at a console:
//   security add-trusted-cert -d /opt/x509/macos-trusted-ca.pem
//   security add-trusted-cert -d -r trustAsRoot /opt/x509/macos-trusted-server.pem
//   security add-trusted-cert -d -r trustAsRoot /opt/x509/macos-trusted-client.pem

load('jstests/libs/python.js');

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;

let ca = 'jstests/libs/ca.pem';
let server = 'jstests/libs/server.pem';
let client = 'jstests/libs/client.pem';

if (HOST_TYPE == "macOS") {
    ca = "/opt/x509/macos-trusted-ca.pem";
    server = "/opt/x509/macos-trusted-server.pem";
    client = "/opt/x509/macos-trusted-client.pem";
    // Ensure the CA is properly installed on MacOS hosts.
    // (MacOS is the only OS where it is installed outside of this test)
    let exitCode = runProgram("security", "verify-cert", "-c", client);
    assert.eq(0, exitCode, "Check for proper installation of Trusted CA on MacOS host");
}
if (_isWindows()) {
    assert.eq(0, runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"));

    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", ca);

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", ca);
}

try {
    var replTest = new ReplSetTest({
        name: "ssltest",
        nodes: 1,
        nodeOptions: {
            sslMode: "requireSSL",
            sslPEMKeyFile: server,
            setParameter: {tlsUseSystemCA: true},
        },
        host: "localhost",
        useHostName: false,
    });

    replTest.startSet({
        env: {
            SSL_CERT_FILE: ca,
        },
    });

    replTest.initiate();

    var nodeList = replTest.nodeList().join();

    var checkShell = function(url) {
        // Should not be able to authenticate with x509.
        // Authenticate call will return 1 on success, 0 on error.
        var argv = ['mongo', url, '--eval', ('db.runCommand({replSetGetStatus: 1})')];

        if (url.endsWith('&ssl=true')) {
            argv.push('--tls', '--tlsCertificateKeyFile', client);
        }

        if (!_isWindows()) {
            // On Linux we override the default path to the system CA store to point to our
            // system CA. On Windows, this CA will have been added to the user's trusted CA list
            argv.unshift("env", "SSL_CERT_FILE=" + ca);
        }
        var ret = runMongoProgram(...argv);
        return ret;
    };

    jsTest.log("Testing with no ssl specification...");
    var noMentionSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}`;
    assert.neq(checkShell(noMentionSSLURL), 0, "shell correctly failed to connect without SSL");

    jsTest.log("Testing with ssl specified false...");
    var disableSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}&ssl=false`;
    assert.neq(checkShell(disableSSLURL), 0, "shell correctly failed to connect without SSL");

    jsTest.log("Testing with ssl specified true...");
    var useSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}&ssl=true`;
    assert.eq(checkShell(useSSLURL), 0, "successfully connected with SSL");

    replTest.stopSet();
} finally {
    if (_isWindows()) {
        const ca_thumbprint = cat('jstests/libs/ca.pem.digest.sha1');
        runProgram("certutil.exe", "-delstore", "-f", "Root", ca_thumbprint);
        runProgram("certutil.exe", "-delstore", "-user", "-f", "CA", ca_thumbprint);
    }
}
