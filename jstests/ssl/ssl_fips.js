load("jstests/ssl/libs/ssl_helpers.js");

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

let expectSupportsFIPS = supportsFIPS();

// if mongo shell didn't start/connect properly
if (mongo != 0) {
    print("mongod failed to start, checking for FIPS support");
    mongoOutput = rawMongoProgramOutput();

    let regexTest =
        /this version of mongodb was not compiled with FIPS support|FIPS_mode_set:fips mode not supported/;

    if (_isWindows()) {
        regexTest = /FIPS modes is not enabled on the operating system/;
    }

    if (isOpenSSL3orGreater()) {
        regexTest = /Failed to load OpenSSL 3 FIPS provider/;
    }

    assert(regexTest.test(mongoOutput));
} else {
    // verify that auth works, SERVER-18051
    md.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]});
    assert(md.getDB("admin").auth("root", "root"), "auth failed");

    // kill mongod
    MongoRunner.stopMongod(md);
}
