// Check that rotation will fail if a certificate file is missing

(function() {
"use strict";

load('jstests/ssl/libs/ssl_helpers.js');

function deleteFile(file) {
    if (_isWindows()) {
        // correctly replace forward slashes for Windows
        file = file.replace(/\//g, "\\");
        assert.eq(0, runProgram("cmd.exe", "/c", "del", file));
        return;
    }
    assert.eq(0, runProgram("rm", file));
}

const dbPath = MongoRunner.toRealDir("$dataDir/cluster_x509_rotate_test/");
mkdir(dbPath);

copyCertificateFile("jstests/libs/ca.pem", dbPath + "/ca-test.pem");
copyCertificateFile("jstests/libs/client.pem", dbPath + "/client-test.pem");
copyCertificateFile("jstests/libs/server.pem", dbPath + "/server-test.pem");
copyCertificateFile("jstests/libs/crl.pem", dbPath + "/crl-test.pem");

const mongod = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: dbPath + "/server-test.pem",
    sslCAFile: dbPath + "/ca-test.pem",
    sslClusterFile: dbPath + "/client-test.pem",
    sslCRLFile: dbPath + "/crl-test.pem",
});

// if we are on apple, don't do delete test on CRL -- it will succeed.
let certTypes = ["server", "ca", "client"];
if (determineSSLProvider() !== "apple") {
    certTypes.push("crl");
}

for (let certType of certTypes) {
    copyCertificateFile("jstests/libs/ca.pem", dbPath + "/ca-test.pem");
    copyCertificateFile("jstests/libs/client.pem", dbPath + "/client-test.pem");
    copyCertificateFile("jstests/libs/server.pem", dbPath + "/server-test.pem");
    copyCertificateFile("jstests/libs/crl.pem", dbPath + "/crl-test.pem");
    assert.commandWorked(mongod.adminCommand({rotateCertificates: 1}));

    deleteFile(`${dbPath}/${certType}-test.pem`);
    assert.commandFailed(mongod.adminCommand({rotateCertificates: 1}));
}

MongoRunner.stopMongod(mongod);
})();
