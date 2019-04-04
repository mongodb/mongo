// Test merizod start with FIPS mode enabled
var port = allocatePort();
var md = MerizoRunner.runMerizod({
    port: port,
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslFIPSMode: ""
});

var merizo = runMerizoProgram("merizo",
                            "--port",
                            port,
                            "--ssl",
                            "--sslAllowInvalidCertificates",
                            "--sslPEMKeyFile",
                            "jstests/libs/client.pem",
                            "--sslFIPSMode",
                            "--eval",
                            ";");

// if merizo shell didn't start/connect properly
if (merizo != 0) {
    print("merizod failed to start, checking for FIPS support");
    merizoOutput = rawMerizoProgramOutput();
    assert(merizoOutput.match(/this version of merizodb was not compiled with FIPS support/) ||
           merizoOutput.match(/FIPS modes is not enabled on the operating system/) ||
           merizoOutput.match(/FIPS_mode_set:fips mode not supported/));
} else {
    // verify that auth works, SERVER-18051
    md.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]});
    assert(md.getDB("admin").auth("root", "root"), "auth failed");

    // kill merizod
    MerizoRunner.stopMerizod(md);
}
