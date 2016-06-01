// Test mongod start with FIPS mode enabled
var port = allocatePort();
var md = MongoRunner.runMongod({
    port: port,
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslFIPSMode: ""
});

var mongo = runMongoProgram("mongo",
                            "--port",
                            port,
                            "--ssl",
                            "--sslAllowInvalidCertificates",
                            "--sslPEMKeyFile",
                            "jstests/libs/client.pem",
                            "--sslFIPSMode",
                            "--eval",
                            ";");

// if mongo shell didn't start/connect properly
if (mongo != 0) {
    print("mongod failed to start, checking for FIPS support");
    mongoOutput = rawMongoProgramOutput();
    assert(mongoOutput.match(/this version of mongodb was not compiled with FIPS support/) ||
           mongoOutput.match(/FIPS_mode_set:fips mode not supported/) ||
           // Ubuntu 16.04's OpenSSL produces an unexpected error message, remove this check when
           // SERVER-24350 is resolved
           mongoOutput.match(/error:00000000:lib\(0\):func\(0\):reason\(0\)/));
} else {
    // verify that auth works, SERVER-18051
    md.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]});
    assert(md.getDB("admin").auth("root", "root"), "auth failed");

    // kill mongod
    MongoRunner.stopMongod(md);
}
