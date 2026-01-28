// Test forcing certificate validation
// This tests that forcing certification validation will prohibit clients without certificates
// from connecting.

// Test that connecting with no client certificate and --tlsAllowConnectionsWithoutCertificates
// (an alias for sslWeakCertificateValidation) connects successfully.
let md = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
    tlsAllowConnectionsWithoutCertificates: "",
});

let mongo = runMongoProgram("mongo", "--port", md.port, "--tls", "--tlsCAFile", getX509Path("ca.pem"), "--eval", ";");

// 0 is the exit code for success
assert(mongo == 0);

// Test that connecting with a valid client certificate connects successfully.
mongo = runMongoProgram(
    "mongo",
    "--port",
    md.port,
    "--tls",
    "--tlsCAFile",
    getX509Path("ca.pem"),
    "--tlsCertificateKeyFile",
    getX509Path("client.pem"),
    "--eval",
    ";",
);

// 0 is the exit code for success
assert(mongo == 0);
MongoRunner.stopMongod(md);
// Test that connecting with no client certificate and no --tlsAllowConnectionsWithoutCertificates
// fails to connect.
let md2 = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
});

mongo = runMongoProgram("mongo", "--port", md2.port, "--tls", "--tlsCAFile", getX509Path("ca.pem"), "--eval", ";");

// 1 is the exit code for failure
assert(mongo == 1);
MongoRunner.stopMongod(md2);
