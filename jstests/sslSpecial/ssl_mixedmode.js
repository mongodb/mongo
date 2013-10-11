// Test the --sslMode parameter
// This tests runs through the 8 possible combinations of sslMode values
// and SSL-enabled and disabled shell respectively. For each combination
// expected behavior is verified. 
var SERVER_CERT = "jstests/libs/server.pem"
var CA_CERT = "jstests/libs/ca.pem" 
var CLIENT_CERT = "jstests/libs/client.pem"

var baseName = "jstests_mixed_mode_ssl"
port = allocatePorts(1)[0];

function testCombination(sslMode, sslShell, shouldSucceed) {
    if (sslMode == "noSSL") {
        MongoRunner.runMongod({port: port});
    }
    else {
        MongoRunner.runMongod({port: port,
                               sslMode: sslMode, 
                               sslPEMKeyFile: SERVER_CERT,
                               sslCAFile: CA_CERT});
    }

    var mongo;
    if (sslShell) {
        mongo = runMongoProgram("mongo", "--port", port, "--ssl", 
                                "--sslPEMKeyFile", CLIENT_CERT,
                                "--eval", ";");
    }
    else {
        mongo = runMongoProgram("mongo", "--port", port,
                                "--eval", ";");
    }

    if (shouldSucceed) {
        // runMongoProgram returns 0 on success
        assert.eq(0, mongo, "Connection attempt failed when it should succeed sslMode:" + 
                  sslMode + " SSL-shell:" + sslShell);
    }
    else {
        // runMongoProgram returns 1 on failure
        assert.eq(1, mongo, "Connection attempt succeeded when it should fail sslMode:" + 
                  sslMode + " SSL-shell:" + sslShell);
    }
    stopMongod(port);
}

testCombination("noSSL", false, true);
testCombination("acceptSSL", false, true);
testCombination("sendAcceptSSL", false, true);
testCombination("sslOnly", false, false);
testCombination("noSSL", true, false);
testCombination("acceptSSL", true, true);
testCombination("sendAcceptSSL", true, true);
testCombination("sslOnly", true, true);

