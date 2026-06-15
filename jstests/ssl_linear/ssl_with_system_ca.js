// On MacOS this test assumes that certificates exist at
// /opt/x509/macos-trusted-[ca|server|client].pem, and that /opt/x509/macos-trusted-ca.pem has
// been added as a trusted certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildhost-configuration/blob/1c1fcb51924cd4f1bc9eaf5db23f6e4365d6ba17/roles/macos/tasks/keychains.yml#L58-L87
// for details.
// To install certificates for local testing on OSX, invoke the following at a console:
//   mkdir /opt/x509
//   python x509/mkcert.py x509/apple_certs.json -o /opt/x509
//   security add-trusted-cert -d /opt/x509/macos-trusted-ca.pem
//   security add-trusted-cert -d -r trustAsRoot /opt/x509/macos-trusted-server.pem
//   security add-trusted-cert -d -r trustAsRoot /opt/x509/macos-trusted-client.pem

import {getPython3Binary} from "jstests/libs/python.js";

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
jsTest.log("HOST_TYPE = " + HOST_TYPE);

let trustedCA = "jstests/libs/trusted-ca.pem";
let trustedServer = "jstests/libs/trusted-server.pem";
let trustedClient = "jstests/libs/trusted-client.pem";

if (HOST_TYPE == "macOS") {
    trustedCA = "/opt/x509/macos-trusted-ca.pem";
    trustedServer = "/opt/x509/macos-trusted-server.pem";
    trustedClient = "/opt/x509/macos-trusted-client.pem";
    // Ensure trustedCA is properly installed on MacOS hosts.
    // (MacOS is the only OS where it is installed outside of this test)
    let exitCode = runProgram("security", "verify-cert", "-c", trustedClient);
    assert.eq(0, exitCode, 'Check for proper installation of Trusted CA on MacOS host');
}
if (HOST_TYPE == "windows") {
    assert.eq(0, runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"));

    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", trustedCA);

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", trustedCA);
}

function testWithCerts(prefix) {
    jsTest.log("Starting mongod blindly...");
    // The trusted certificates come from the system certificate store (on MacOS, these are the
    // provision-time generated certs); the untrusted control certs always come from jstests/libs.
    const isTrusted = prefix === 'trusted-';
    const serverCert = isTrusted ? trustedServer : 'jstests/libs/' + prefix + 'server.pem';
    const clientCert = isTrusted ? trustedClient : 'jstests/libs/' + prefix + 'client.pem';
    // allowTLS to get a non-TLS control connection.
    var opts = {
        tlsMode: 'preferTLS',
        tlsCertificateKeyFile: serverCert,
        waitForConnect: false,
        setParameter: {tlsUseSystemCA: true},
        env: {"SSL_CERT_FILE": trustedCA},
    };
    const conn = MongoRunner.runMongod(opts);

    jsTest.log("Waiting for mongod to be non-TLS connectable...");
    let argv = ['mongo', '--port', conn.port, '--eval', ';'];

    assert.soon((exitCode) => {
        exitCode = runMongoProgram.apply(null, argv);
        return 0 == exitCode;
    });

    jsTest.log("Testing connection with " + clientCert + " ...");
    argv = [
        'mongo',
        '--tls',
        '--port',
        conn.port,
        '--tlsCertificateKeyFile',
        clientCert,
        '--eval',
        ';'
    ];

    if (HOST_TYPE == "linux") {
        // On Linux we override the default path to the system CA store to point to our
        // "trusted" CA. On Windows, this CA will have been added to the user's trusted CA list
        argv.unshift("env", "SSL_CERT_FILE=" + trustedCA);
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
