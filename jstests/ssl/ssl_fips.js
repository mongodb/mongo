// Test mongod start with FIPS mode enabled
ports = allocatePorts(1);
port1 = ports[0];
var md = MongoRunner.runMongod({port: port1,
                                sslMode: "requireSSL",
                                sslPEMKeyFile: "jstests/libs/server.pem",
                                sslCAFile: "jstests/libs/ca.pem",
                                sslFIPSMode: ""});

var mongo = runMongoProgram("mongo",
                            "--port", port1,
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
    // verify that auth works, SERVER-18051
    md.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]});
    assert(md.getDB("admin").auth("root", "root"), "auth failed");

    // kill mongod
    MongoRunner.stopMongod(md);
}
