// Test SSL server certificate hostname validation
// for client-server and server-server connections
var CA_CERT = "jstests/libs/ca.pem";
var SERVER_CERT = "jstests/libs/server.pem";
var CN_CERT = "jstests/libs/localhostnameCN.pem";
var SAN_CERT = "jstests/libs/localhostnameSAN.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var BAD_SAN_CERT = "jstests/libs/badSAN.pem";

function testCombination(certPath, allowInvalidHost, allowInvalidCert, shouldSucceed) {
    var mongod =
        MongoRunner.runMongod({sslMode: "requireSSL", sslPEMKeyFile: certPath, sslCAFile: CA_CERT});

    var mongo;
    if (allowInvalidCert) {
        mongo = runMongoProgram("mongo",
                                "--port",
                                mongod.port,
                                "--ssl",
                                "--sslCAFile",
                                CA_CERT,
                                "--sslPEMKeyFile",
                                CLIENT_CERT,
                                "--sslAllowInvalidCertificates",
                                "--eval",
                                ";");
    } else if (allowInvalidHost) {
        mongo = runMongoProgram("mongo",
                                "--port",
                                mongod.port,
                                "--ssl",
                                "--sslCAFile",
                                CA_CERT,
                                "--sslPEMKeyFile",
                                CLIENT_CERT,
                                "--sslAllowInvalidHostnames",
                                "--eval",
                                ";");
    } else {
        mongo = runMongoProgram("mongo",
                                "--port",
                                mongod.port,
                                "--ssl",
                                "--sslCAFile",
                                CA_CERT,
                                "--sslPEMKeyFile",
                                CLIENT_CERT,
                                "--eval",
                                ";");
    }

    if (shouldSucceed) {
        // runMongoProgram returns 0 on success
        assert.eq(
            0, mongo, "Connection attempt failed when it should succeed certPath: " + certPath);
    } else {
        // runMongoProgram returns 1 on failure
        assert.eq(
            1, mongo, "Connection attempt succeeded when it should fail certPath: " + certPath);
    }
    MongoRunner.stopMongod(mongod.port);
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

// 2. Initiate ReplSetTest with invalid certs
ssl_options = {
    sslMode: "requireSSL",
    // SERVER_CERT has SAN=localhost. CLIENT_CERT is exact same except no SANS
    sslPEMKeyFile: CLIENT_CERT,
    sslCAFile: CA_CERT
};

replTest = new ReplSetTest({nodes: {node0: ssl_options, node1: ssl_options}});
replTest.startSet();
assert.throws(function() {
    replTest.initiate();
});
replTest.stopSet();

// 3. Initiate ReplSetTest with invalid certs but set allowInvalidHostnames
ssl_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT,
    sslAllowInvalidHostnames: ""
};

var replTest = new ReplSetTest({nodes: {node0: ssl_options, node1: ssl_options}});
replTest.startSet();
replTest.initiate();
replTest.stopSet();

// 4. Initiate ReplSetTest with invalid certs but set allowInvalidCertificates
ssl_options = {
    sslMode: "requireSSL",
    // SERVER_CERT has SAN=localhost. CLIENT_CERT is exact same except no SANS
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT,
    sslAllowInvalidCertificates: ""
};

var replTest = new ReplSetTest({nodes: {node0: ssl_options, node1: ssl_options}});
replTest.startSet();
replTest.initiate();
replTest.stopSet();
