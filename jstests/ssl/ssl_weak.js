// Test forcing certificate validation
// This tests that forcing certification validation will prohibit clients without certificates
// from connecting.

// Test that connecting with no client certificate and --sslAllowConnectionsWithoutCertificates
// (an alias for sslWeakCertificateValidation) connects successfully.
var md = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslAllowConnectionsWithoutCertificates: ""
});

var mongo = runMongoProgram(
    "mongo", "--port", md.port, "--ssl", "--sslAllowInvalidCertificates", "--eval", ";");

// 0 is the exit code for success
assert(mongo == 0);

// Test that connecting with a valid client certificate connects successfully.
mongo = runMongoProgram("mongo",
                        "--port",
                        md.port,
                        "--ssl",
                        "--sslAllowInvalidCertificates",
                        "--sslPEMKeyFile",
                        "jstests/libs/client.pem",
                        "--eval",
                        ";");

// 0 is the exit code for success
assert(mongo == 0);

// Test that connecting with no client certificate and no --sslAllowConnectionsWithoutCertificates
// fails to connect.
var md2 = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem"
});

mongo = runMongoProgram(
    "mongo", "--port", md2.port, "--ssl", "--sslAllowInvalidCertificates", "--eval", ";");

// 1 is the exit code for failure
assert(mongo == 1);
