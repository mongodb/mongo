// Test --host.
(function() {
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

        // Pass the inner test's exit code back as the outer test's exit code
        if (exitCode != 0) {
            doassert("inner test failed with exit code " + exitcode);
        }
        return;
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
                doassert("successfully connected with `--host " + host +
                         "`, but expected to fail.");
            }
        }
    };

    var goodStrings = [
        "[::1]:27999",
        "localhost:27999",
        "127.0.0.1:27999",
        "[0:0:0:0:0:0:0:1]:27999",
        "[0000:0000:0000:0000:0000:0000:0000:0001]:27999",
    ];

    var goodSocketStrings = [
        "/tmp/mongodb-27999.sock",
    ];

    var badStrings = [
        "::1:27999",
        "::1:65536",
        "::1]:27999",
        ":",
        ":27999",
        "[::1:]27999",
        "[::1:27999",
        "[::1]:",
        "[::1]:123456",
        "[::1]:1cat",
        "[::1]:65536",
        "[::1]:cat",
        "0:0::0:0:1:27999",
        "0000:0000:0000:0000:0000:0000:0000:0001:27999",
        "127.0.0.1:",
        "127.0.0.1:123456",
        "127.0.0.1:1cat",
        "127.0.0.1:65536",
        "127.0.0.1:cat",
        "a[::1:]27999",
        "a[127.0.0.1]:27999",
        "localhost:",
    ];

    function runUriTestFor(i, connectionString, isGood) {
        connectionString = connectionString.replace("27999", "" + port);
        print("Testing " + (isGood ? "good" : "bad") + " connection string " + i + "...");
        print("    * testing " + connectionString);
        testHost(connectionString, isGood);
        print("    * testing mongodb://" + encodeURIComponent(connectionString));
        testHost("mongodb://" + encodeURIComponent(connectionString), isGood);
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
})();
