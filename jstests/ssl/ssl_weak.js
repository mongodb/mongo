// Test forcing certificate validation
// This tests that forcing certification validation will prohibit clients without certificates
// from connecting.

// Test that connecting with no client certificate and --tlsAllowConnectionsWithoutCertificates
// (an alias for sslWeakCertificateValidation) connects successfully.
var md = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    tlsAllowConnectionsWithoutCertificates: ""
});

var mongo = runMongoProgram(
    "mongo", "--port", md.port, "--tls", "--tlsCAFile", "jstests/libs/ca.pem", "--eval", ";");

// 0 is the exit code for success
assert(mongo == 0);

// Test that connecting with a valid client certificate connects successfully.
mongo = runMongoProgram("mongo",
                        "--port",
                        md.port,
                        "--tls",
                        "--tlsCAFile",
                        "jstests/libs/ca.pem",
                        "--tlsCertificateKeyFile",
                        "jstests/libs/client.pem",
                        "--eval",
                        ";");

// 0 is the exit code for success
assert(mongo == 0);
MongoRunner.stopMongod(md);
// Test that connecting with no client certificate and no --tlsAllowConnectionsWithoutCertificates
// fails to connect.
var md2 = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem"
});

mongo = runMongoProgram(
    "mongo", "--port", md2.port, "--tls", "--tlsCAFile", "jstests/libs/ca.pem", "--eval", ";");

// 1 is the exit code for failure
assert(mongo == 1);
MongoRunner.stopMongod(md2);
