// On OSX this test assumes that jstests/libs/trusted-ca.pem has been added as a trusted
// certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildslave-cookbooks/commit/af7cabe5b6e0885902ebd4902f7f974b64cc8961
// for details.
// To install trusted-ca.pem for local testing on OSX, invoke the following at a console:
//   security add-trusted-cert -d jstests/libs/trusted-ca.pem
(function() {
'use strict';

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
if (HOST_TYPE == "windows") {
    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", "jstests\\libs\\trusted-ca.pem");

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", "jstests\\libs\\trusted-ca.pem");
}

const x509Options = {
    sslMode: 'requireSSL',
    sslPEMKeyFile: 'jstests/libs/trusted-server.pem',
    sslCAFile: 'jstests/libs/trusted-ca.pem',
    sslAllowInvalidCertificates: '',
    sslWeakCertificateValidation: '',
};

const rst = new ReplSetTest(
    {nodes: 2, name: "sslSet", useHostName: false, nodeOptions: x509Options, waitForKeys: false});
rst.startSet();
rst.initiate();

const subShellCommand = function(hosts) {
    var Ms = [];
    for (var i = 0; i < 10; i++) {
        Ms.push(
            new Mongo("mongodb://" + hosts[0] + "," + hosts[1] + "/?ssl=true&replicaSet=sslSet"));
    }

    for (var i = 0; i < 10; i++) {
        var db = Ms[i].getDB("test");
        db.setSlaveOk(true);
        db.col.find().readPref("secondary").toArray();
    }
};

const subShellCommandFormatter = function(replSet) {
    var hosts = [];
    replSet.nodes.forEach((node) => {
        hosts.push("localhost:" + node.port);
    });

    let command = `
            (function () {
                'use strict';
                let command = ${subShellCommand.toString()};
                let hosts = ${tojson(hosts)};
                command(hosts);
            }());`;

    return command;
};

const subShellArgs = [
    "env",
    "SSL_CERT_FILE=jstests/libs/trusted-ca.pem",
    './mongo',
    '--nodb',
    '--eval',
    subShellCommandFormatter(rst)
];

const retVal = _runMongoProgram(...subShellArgs);
assert.eq(retVal, 0, 'mongo shell did not succeed with exit code 0');

rst.stopSet();
}());
