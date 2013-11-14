// Test SSL server certificate hostname validation
// for client-server and server-server connections 
var CA_CERT = "jstests/libs/ca.pem" 
var SERVER_CERT = "jstests/libs/server.pem";
var CN_CERT = "jstests/libs/localhostnameCN.pem"; 
var SAN_CERT = "jstests/libs/localhostnameSAN.pem"; 
var CLIENT_CERT = "jstests/libs/client.pem"

// We want to be able to control all SSL parameters
// but still need an SSL shell hence the test is placed
// in the /ssl directory
TestData.useX509 = false;
TestData.useSSL = false;

port = allocatePorts(1)[0];

function testCombination(certPath, allowInvalidCert, shouldSucceed) {
    MongoRunner.runMongod({port: port,
                           sslMode: "requireSSL", 
                           sslPEMKeyFile: certPath,
                           sslCAFile: CA_CERT});

    var mongo;
    if (allowInvalidCert) {
        mongo = runMongoProgram("mongo", "--port", port, "--ssl", 
                                "--sslCAFile", CA_CERT, 
                                "--sslPEMKeyFile", CLIENT_CERT,
                                "--sslAllowInvalidCertificates",
                                "--eval", ";");
    }
    else { 
        mongo = runMongoProgram("mongo", "--port", port, "--ssl", 
                                "--sslCAFile", CA_CERT, 
                                "--sslPEMKeyFile", CLIENT_CERT,
                                "--eval", ";");
    }

    if (shouldSucceed) {
        // runMongoProgram returns 0 on success
        assert.eq(0, mongo, "Connection attempt failed when it should succeed certPath: " + 
                  certPath);
    }
    else {
        // runMongoProgram returns 1 on failure
        assert.eq(1, mongo, "Connection attempt succeeded when it should fail certPath: " + 
                  certPath);
    }
    stopMongod(port);
}

// 1. Test client connections with different server certificates
// and allowInvalidCertificates
testCombination(CN_CERT, false, true);
testCombination(SAN_CERT, false, true);
testCombination(SERVER_CERT, false, false);
testCombination(SERVER_CERT, true, true);

// 2. Initiate ReplSetTest with invalid certs
ssl_options = {sslMode : "requireSSL",
               sslPEMKeyFile : SERVER_CERT,
               sslCAFile: CA_CERT};

replTest = new ReplSetTest({nodes : {node0 : ssl_options, node1 : ssl_options}});
replTest.startSet();
assert.throws( function() { replTest.initiate() } );
replTest.stopSet();

// 3. Initiate ReplSetTest with invalid certs but set allowInvalidCertificates
ssl_options = {sslMode : "requireSSL",
               sslPEMKeyFile : SERVER_CERT,
               sslCAFile: CA_CERT,
               sslAllowInvalidCertificates: ""};

var replTest = new ReplSetTest({nodes : {node0 : ssl_options, node1 : ssl_options}});
replTest.startSet();
replTest.initiate();
replTest.stopSet();
