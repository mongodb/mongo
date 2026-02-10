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
import {ReplSetTest} from "jstests/libs/replsettest.js";

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
jsTest.log("HOST_TYPE = " + HOST_TYPE);

let trustedCA = getX509Path("trusted-ca.pem");
let trustedServer = getX509Path("trusted-server.pem");
let trustedClient = getX509Path("trusted-client.pem");

if (HOST_TYPE == "macOS") {
    trustedCA = "/opt/x509/macos-trusted-ca.pem";
    trustedServer = "/opt/x509/macos-trusted-server.pem";
    trustedClient = "/opt/x509/macos-trusted-client.pem";
    // Ensure trustedCA is properly installed on MacOS hosts.
    // (MacOS is the only OS where it is installed outside of this test)
    let exitCode = runProgram("security", "verify-cert", "-c", trustedClient);
    assert.eq(0, exitCode, "Check for proper installation of Trusted CA on MacOS host");
}
if (HOST_TYPE == "windows") {
    assert.eq(0, runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"));

    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", trustedCA);

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", trustedCA);
}

try {
    let replTest = new ReplSetTest({
        name: "TLSTest",
        nodes: 1,
        nodeOptions: {
            tlsMode: "requireTLS",
            tlsCertificateKeyFile: trustedServer,
            setParameter: {tlsUseSystemCA: true},
        },
        host: "localhost",
        useHostName: false,
    });

    replTest.startSet({
        env: {
            SSL_CERT_FILE: trustedCA,
        },
    });

    replTest.initiate();

    let nodeList = replTest.nodeList().join();

    let checkShell = function (url) {
        // Should not be able to authenticate with x509.
        // Authenticate call will return 1 on success, 0 on error.
        let argv = ["mongo", url, "--eval", "db.runCommand({replSetGetStatus: 1})"];

        if (url.endsWith("&ssl=true")) {
            argv.push("--tls", "--tlsCertificateKeyFile", trustedClient);
        }

        if (!_isWindows()) {
            // On Linux we override the default path to the system CA store to point to our
            // system CA. On Windows, this CA will have been added to the user's trusted CA list
            argv.unshift("env", "SSL_CERT_FILE=" + trustedCA);
        }
        let ret = runMongoProgram(...argv);
        return ret;
    };

    jsTest.log("Testing with no ssl specification...");
    let noMentionSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}`;
    assert.neq(checkShell(noMentionSSLURL), 0, "shell correctly failed to connect without SSL");

    jsTest.log("Testing with ssl specified false...");
    let disableSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}&ssl=false`;
    assert.neq(checkShell(disableSSLURL), 0, "shell correctly failed to connect without SSL");

    jsTest.log("Testing with ssl specified true...");
    let useSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}&ssl=true`;
    assert.eq(checkShell(useSSLURL), 0, "successfully connected with SSL");

    replTest.stopSet();
} finally {
    if (_isWindows()) {
        const ca_thumbprint = cat(getX509Path("trusted-ca.pem.digest.sha1"));
        runProgram("certutil.exe", "-delstore", "-f", "Root", ca_thumbprint);
        runProgram("certutil.exe", "-delstore", "-user", "-f", "CA", ca_thumbprint);
    }
}
