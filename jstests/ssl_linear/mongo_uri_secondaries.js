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
    const x509Options = {
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: "jstests/libs/trusted-server.pem",
        tlsCAFile: "jstests/libs/trusted-ca.pem",
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

    const retVal = runWithEnv(subShellArgs, {"SSL_CERT_FILE": "jstests/libs/trusted-ca.pem"});
    assert.eq(retVal, 0, "mongo shell did not succeed with exit code 0");

    rst.stopSet();
} finally {
    if (HOST_TYPE == "windows") {
        const trusted_ca_thumbprint = cat("jstests/libs/trusted-ca.pem.digest.sha1");
        runProgram("certutil.exe", "-delstore", "-f", "Root", trusted_ca_thumbprint);
        runProgram("certutil.exe", "-delstore", "-user", "-f", "CA", trusted_ca_thumbprint);
    }
}
