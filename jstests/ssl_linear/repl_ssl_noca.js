// On OSX this test assumes that jstests/libs/trusted-ca.pem has been added as a trusted
// certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildslave-cookbooks/commit/af7cabe5b6e0885902ebd4902f7f974b64cc8961
// for details.
// To install trusted-ca.pem for local testing on OSX, invoke the following at a console:
//   security add-trusted-cert -d jstests/libs/trusted-ca.pem

import {getPython3Binary} from "jstests/libs/python.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
jsTest.log("HOST_TYPE = " + HOST_TYPE);

if (HOST_TYPE == "macOS") {
    // Ensure trusted-ca.pem is properly installed on MacOS hosts.
    // (MacOS is the only OS where it is installed outside of this test)
    let exitCode = runProgram("security", "verify-cert", "-c", "./jstests/libs/trusted-client.pem");
    assert.eq(0, exitCode, "Check for proper installation of Trusted CA on MacOS host");
}
if (HOST_TYPE == "windows") {
    assert.eq(0, runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"));

    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", "jstests\\libs\\trusted-ca.pem");

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", "jstests\\libs\\trusted-ca.pem");
}

try {
    let replTest = new ReplSetTest({
        name: "TLSTest",
        nodes: 1,
        nodeOptions: {
            tlsMode: "requireTLS",
            tlsCertificateKeyFile: "jstests/libs/trusted-server.pem",
            setParameter: {tlsUseSystemCA: true},
        },
        host: "localhost",
        useHostName: false,
    });

    replTest.startSet({
        env: {
            SSL_CERT_FILE: "jstests/libs/trusted-ca.pem",
        },
    });

    replTest.initiate();

    let nodeList = replTest.nodeList().join();

    let checkShell = function (url) {
        // Should not be able to authenticate with x509.
        // Authenticate call will return 1 on success, 0 on error.
        let argv = ["mongo", url, "--eval", "db.runCommand({replSetGetStatus: 1})"];

        if (url.endsWith("&ssl=true")) {
            argv.push("--tls", "--tlsCertificateKeyFile", "jstests/libs/trusted-client.pem");
        }

        if (!_isWindows()) {
            // On Linux we override the default path to the system CA store to point to our
            // system CA. On Windows, this CA will have been added to the user's trusted CA list
            argv.unshift("env", "SSL_CERT_FILE=jstests/libs/trusted-ca.pem");
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
        const ca_thumbprint = cat("jstests/libs/trusted-ca.pem.digest.sha1");
        runProgram("certutil.exe", "-delstore", "-f", "Root", ca_thumbprint);
        runProgram("certutil.exe", "-delstore", "-user", "-f", "CA", ca_thumbprint);
    }
}
