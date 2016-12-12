// Test --host.

// This "inner_mode" method of spawning a mongod and re-running was copied from
// ipv6_connection_string_validation.js
if ("undefined" == typeof inner_mode) {
    // Start a mongod with --ipv6
    jsTest.log("Outer mode test starting mongod with --ipv6");
    // NOTE: bind_ip arg is present to test if it can parse ipv6 addresses (::1 in this case).
    // Unfortunately, having bind_ip = ::1 won't work in the test framework (But does work when
    // tested manually), so 127.0.0.1 is also present so the test mongo shell can connect
    // with that address.
    var mongod = MongoRunner.runMongod({ipv6: "", bind_ip: "::1,127.0.0.1"});
    var args = [
        "mongo",
        "--nodb",
        "--ipv6",
        "--host",
        "::1",
        "--port",
        mongod.port,
        "--eval",
        "inner_mode=true;port=" + mongod.port + ";",
        "jstests/noPassthroughWithMongod/host_connection_string_validation.js"
    ];
    var exitCode = _runMongoProgram.apply(null, args);
    jsTest.log("Inner mode test finished, exit code was " + exitCode);

    // Stop the server we started
    jsTest.log("Outer mode test stopping server");
    MongoRunner.stopMongod(mongod.port, 15);

    // Pass the inner test's exit code back as the outer test's exit code
    quit(exitCode);
}

var testHost = function(host, shouldSucceed) {
    var exitCode = runMongoProgram('mongo', '--ipv6', '--eval', ';', '--host', host);
    if (shouldSucceed) {
        if (exitCode !== 0) {
            doassert("failed to connect with `--host " + host +
                     "`, but expected success. Exit code: " + exitCode);
        }
    } else {
        if (exitCode === 0) {
            doassert("successfully connected with `--host " + host + "`, but expected to fail.");
        }
    }
};

var goodStrings = [
    "[::1]:27999",
    "[::1]:27999/test",
    "localhost:27999",
    "localhost:27999/test",
    "127.0.0.1:27999",
    "127.0.0.1:27999/test",
    "[0:0:0:0:0:0:0:1]:27999",
    "[0:0:0:0:0:0:0:1]:27999/test",
    "[0000:0000:0000:0000:0000:0000:0000:0001]:27999",
    "[0000:0000:0000:0000:0000:0000:0000:0001]:27999/test",
];

var goodSocketStrings = [
    "/tmp/mongodb-27999.sock",
    "/tmp/mongodb-27999.sock/test",
];

var badStrings = [
    "/",
    ":",
    ":/",
    "/test",
    ":/test",
    ":27999/",
    ":27999/test",
    "::1]:27999/",
    "[::1:27999/",
    "[::1]:/test",
    "[::1:]27999/",
    "a[::1:]27999/",
    "::1:27999/test",
    "::1:65536/test",
    "[::1]:cat/test",
    "[::1]:1cat/test",
    "localhost:/test",
    "127.0.0.1:/test",
    "[::1]:65536/test",
    "[::1]:123456/test",
    "127.0.0.1:cat/test",
    "a[127.0.0.1]:27999/",
    "127.0.0.1:1cat/test",
    "127.0.0.1:65536/test",
    "0:0::0:0:1:27999/test",
    "127.0.0.1:123456/test",
    "0000:0000:0000:0000:0000:0000:0000:0001:27999/test",
];

function runUriTestFor(i, connectionString, isGood) {
    connectionString = connectionString.replace("27999", "" + port);
    print("Testing " + (isGood ? "good" : "bad") + " connection string " + i + "...");
    print("    * testing " + connectionString);
    testHost(connectionString, isGood);
    print("    * testing mongodb://" + connectionString);
    testHost("mongodb://" + connectionString, isGood);
}

var i;
jsTest.log("TESTING " + goodStrings.length + " good uri strings");
for (i = 0; i < goodStrings.length; ++i) {
    runUriTestFor(i, goodStrings[i], true);
}

if (!_isWindows()) {
    jsTest.log("TESTING " + goodSocketStrings.length + " good uri socket strings");
    for (i = 0; i < goodSocketStrings.length; ++i) {
        runUriTestFor(i, goodSocketStrings[i], true);
    }
}

jsTest.log("TESTING " + badStrings.length + " bad uri strings");
for (i = 0; i < badStrings.length; ++i) {
    runUriTestFor(i, badStrings[i], false);
}

jsTest.log("SUCCESSFUL test completion");
