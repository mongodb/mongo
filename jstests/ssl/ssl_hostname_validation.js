// Test SSL server certificate hostname validation
// for client-server and server-server connections
import {ReplSetTest} from "jstests/libs/replsettest.js";

let CA_CERT = "jstests/libs/ca.pem";
let SERVER_CERT = "jstests/libs/server.pem";
let CN_CERT = "jstests/libs/localhostnameCN.pem";
let SAN_CERT = "jstests/libs/localhostnameSAN.pem";
let CLIENT_CERT = "jstests/libs/client.pem";
let BAD_SAN_CERT = "jstests/libs/badSAN.pem";
let NOSUBJ_CERT = "jstests/libs/server_no_subject.pem";
let NOSUBJ_NOSAN_CERT = "jstests/libs/server_no_subject_no_SAN.pem";

function testCombination(certPath, allowInvalidHost, allowInvalidCert, shouldSucceed) {
    jsTestLog("Testing certificate: " + JSON.stringify(arguments));
    let mongod = MongoRunner.runMongod({tlsMode: "requireTLS", tlsCertificateKeyFile: certPath, tlsCAFile: CA_CERT});

    let mongo;
    if (allowInvalidCert) {
        mongo = runMongoProgram(
            "mongo",
            "--port",
            mongod.port,
            "--tls",
            "--tlsCAFile",
            CA_CERT,
            "--tlsCertificateKeyFile",
            CLIENT_CERT,
            "--tlsAllowInvalidCertificates",
            "--eval",
            ";",
        );
    } else if (allowInvalidHost) {
        mongo = runMongoProgram(
            "mongo",
            "--port",
            mongod.port,
            "--tls",
            "--tlsCAFile",
            CA_CERT,
            "--tlsCertificateKeyFile",
            CLIENT_CERT,
            "--tlsAllowInvalidHostnames",
            "--eval",
            ";",
        );
    } else {
        mongo = runMongoProgram(
            "mongo",
            "--port",
            mongod.port,
            "--tls",
            "--tlsCAFile",
            CA_CERT,
            "--tlsCertificateKeyFile",
            CLIENT_CERT,
            "--eval",
            ";",
        );
    }

    if (shouldSucceed) {
        // runMongoProgram returns 0 on success
        assert.eq(0, mongo, "Connection attempt failed when it should succeed certPath: " + certPath);
    } else {
        // runMongoProgram returns 1 on failure
        assert.eq(1, mongo, "Connection attempt succeeded when it should fail certPath: " + certPath);
    }
    MongoRunner.stopMongod(mongod);
}

// 1. Test client connections with different server certificates
// and allowInvalidCertificates
testCombination(CN_CERT, false, false, true);
testCombination(SAN_CERT, false, false, true);

// SERVER_CERT has SAN=localhost
testCombination(SERVER_CERT, false, false, true);
testCombination(SERVER_CERT, false, true, true);
testCombination(SERVER_CERT, true, false, true);
testCombination(SERVER_CERT, true, true, true);

// BAD_SAN_CERT has SAN=BadSAN.
testCombination(BAD_SAN_CERT, false, false, false);

// NOSUBJ_CERT has SAN=localhost but empty Subject
testCombination(NOSUBJ_CERT, false, false, true);

// NOSUBJ_NOSAN_CERT has neither Subject nor SANs
testCombination(NOSUBJ_NOSAN_CERT, false, false, false);

// Skip db hash check because replset cannot initiate.
TestData.skipCheckDBHashes = true;

// 2. Initiate ReplSetTest with invalid certs
let ssl_options = {
    tlsMode: "requireTLS",
    // SERVER_CERT has SAN=localhost. CLIENT_CERT is exact same except no SANS
    tlsCertificateKeyFile: CLIENT_CERT,
    tlsCAFile: CA_CERT,
};

replTest = new ReplSetTest({nodes: {node0: ssl_options, node1: ssl_options}});

// We don't want to invoke the hang analyzer because we
// expect this test to fail by timing out
MongoRunner.runHangAnalyzer.disable();

replTest.startSet();
assert.throws(function () {
    replTest.initiate();
});
replTest.stopSet();

// Re-enable the hang analyzer for the test
MongoRunner.runHangAnalyzer.enable();

TestData.skipCheckDBHashes = false;

// 3. Initiate ReplSetTest with invalid certs but set allowInvalidHostnames
ssl_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsAllowInvalidHostnames: "",
};

var replTest = new ReplSetTest({nodes: {node0: ssl_options, node1: ssl_options}});
replTest.startSet();
replTest.initiate();
replTest.stopSet();

// 4. Initiate ReplSetTest with invalid certs but set allowInvalidCertificates
ssl_options = {
    tlsMode: "requireTLS",
    // SERVER_CERT has SAN=localhost. CLIENT_CERT is exact same except no SANS
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsAllowInvalidCertificates: "",
};

replTest = new ReplSetTest({nodes: {node0: ssl_options, node1: ssl_options}});
replTest.startSet();
replTest.initiate();
replTest.stopSet();
