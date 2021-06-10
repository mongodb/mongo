(function() {

// Test mongod start with FIPS mode enabled
const port = allocatePort();
let md = undefined;
try {
    md = MongoRunner.runMongod({
        port: port,
        sslMode: "requireSSL",
        sslPEMKeyFile: "jstests/libs/server.pem",
        sslCAFile: "jstests/libs/ca.pem",
        sslFIPSMode: ""
    });
} catch (e) {
    print("mongod failed to start, checking for FIPS support");
    let mongoOutput = rawMongoProgramOutput();
    assert(mongoOutput.match(/this version of mongodb was not compiled with FIPS support/) ||
           mongoOutput.match(/FIPS modes is not enabled on the operating system/) ||
           mongoOutput.match(/FIPS_mode_set:fips mode not supported/));
    return;
}
assert(md);

// verify that auth works, SERVER-18051
md.getDB("admin").createUser({user: "root", pwd: "root", roles: ["root"]});
assert(md.getDB("admin").auth("root", "root"), "auth failed");

// kill mongod
MongoRunner.stopMongod(md);
})();
