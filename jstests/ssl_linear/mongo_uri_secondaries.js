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
    const x509Options = {
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: trustedServer,
        tlsCAFile: trustedCA,
        tlsClusterFile: trustedClient,
        tlsAllowInvalidCertificates: "",
        tlsWeakCertificateValidation: "",
    };

    const rst = new ReplSetTest({
        nodes: 2,
        name: "tlsSet",
        useHostName: false,
        nodeOptions: x509Options,
        waitForKeys: false,
    });
    rst.startSet();
    rst.initiate();

    const subShellCommand = function (hosts) {
        let Ms = [];
        for (let i = 0; i < 10; i++) {
            Ms.push(new Mongo("mongodb://" + hosts[0] + "," + hosts[1] + "/?ssl=true&replicaSet=tlsSet"));
        }

        for (let i = 0; i < 10; i++) {
            let db = Ms[i].getDB("test");
            db.setSecondaryOk();
            db.col.find().readPref("secondary").toArray();
        }
    };

    const subShellCommandFormatter = function (replSet) {
        let hosts = [];
        replSet.nodes.forEach((node) => {
            hosts.push("localhost:" + node.port);
        });

        let command = `
                (function () {
                    let command = ${subShellCommand.toString()};
                    let hosts = ${tojson(hosts)};
                    command(hosts);
                }());`;

        return command;
    };

    function runWithEnv(args, env) {
        const pid = _startMongoProgram({args: args, env: env});
        return waitProgram(pid);
    }

    const subShellArgs = ["mongo", "--nodb", "--eval", subShellCommandFormatter(rst)];

    const retVal = runWithEnv(subShellArgs, {"SSL_CERT_FILE": trustedCA});
    assert.eq(retVal, 0, "mongo shell did not succeed with exit code 0");

    rst.stopSet();
} finally {
    if (HOST_TYPE == "windows") {
        const trusted_ca_thumbprint = cat(getX509Path("trusted-ca.pem.digest.sha1"));
        runProgram("certutil.exe", "-delstore", "-f", "Root", trusted_ca_thumbprint);
        runProgram("certutil.exe", "-delstore", "-user", "-f", "CA", trusted_ca_thumbprint);
    }
}
