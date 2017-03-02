// Test bongod start with FIPS mode enabled
var port = allocatePort();
var md = BongoRunner.runBongod({
    port: port,
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslFIPSMode: ""
});

var bongo = runBongoProgram("bongo",
                            "--port",
                            port,
                            "--ssl",
                            "--sslAllowInvalidCertificates",
                            "--sslPEMKeyFile",
                            "jstests/libs/client.pem",
                            "--sslFIPSMode",
                            "--eval",
                            ";");

// if bongo shell didn't start/connect properly
if (bongo != 0) {
    print("bongod failed to start, checking for FIPS support");
    bongoOutput = rawBongoProgramOutput();
    assert(bongoOutput.match(/this version of bongodb was not compiled with FIPS support/) ||
           bongoOutput.match(/FIPS_mode_set:fips mode not supported/));
} else {
    // verify that auth works, SERVER-18051
    md.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]});
    assert(md.getDB("admin").auth("root", "root"), "auth failed");

    // kill bongod
    BongoRunner.stopBongod(md);
}
