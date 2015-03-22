// Test mongod start with FIPS mode enabled

var md = MongoRunner.runMongod({sslMode: "requireSSL",
                                sslPEMKeyFile: "jstests/libs/server.pem",
                                sslCAFile: "jstests/libs/ca.pem",
                                sslFIPSMode: ""});

var mongo = runMongoProgram("mongo",
                            "--port", md.port,
                            "--ssl",
                            "--sslAllowInvalidCertificates",
                            "--sslPEMKeyFile", "jstests/libs/client.pem",
                            "--sslFIPSMode",
                            "--eval", ";");

// if mongo shell didn't start/connect properly
if (mongo != 0) {
    print("mongod failed to start, checking for FIPS support");
    assert(rawMongoProgramOutput().match(
            /this version of mongodb was not compiled with FIPS support/));
}
else {
    // kill mongod
    MongoRunner.stopMongod(md);
}
