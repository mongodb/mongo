// Test validation of IPv6 connection strings passed to the JavaScript "connect()" function.
// Related to SERVER-8030.

// This file runs in two modes: outer and inner.  This is to enable testing with --ipv6.
// The outer mode test starts a mongod with --ipv6 and then starts a mongo shell with --ipv6
// and a command line to run the test in inner_mode.  The inner mode test is the actual test.
(function() {
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
            "jstests/noPassthroughWithMongod/ipv6_connection_string_validation.js"
        ];
        var exitCode = _runMongoProgram.apply(null, args);
        jsTest.log("Inner mode test finished, exit code was " + exitCode);

        // Pass the inner test's exit code back as the outer test's exit code
        if (exitCode != 0) {
            doassert("inner test failed with exit code " + exitcode);
        }
        return;
    }

    var goodStrings = [
        "localhost:27999/test",
        "[::1]:27999/test",
        "[0:0:0:0:0:0:0:1]:27999/test",
        "[0000:0000:0000:0000:0000:0000:0000:0001]:27999/test",
        "localhost:27999",
        "[::1]:27999",
        "[0:0:0:0:0:0:0:1]:27999",
        "[0000:0000:0000:0000:0000:0000:0000:0001]:27999",
    ];

    var missingConnString = /^Missing connection string$/;
    var incorrectType = /^Incorrect type/;
    var emptyConnString = /^Empty connection string$/;
    var badHost = /^Failed to parse mongodb/;
    var emptyHost = /^Empty host component/;
    var noPort = /^No digits/;
    var badPort = /^Bad digit/;
    var invalidPort = /^Port number \d+ out of range/;
    var moreThanOneColon = /^More than one ':' detected/;
    var charBeforeSquareBracket = /^'\[' present, but not first character/;
    var noCloseBracket = /^ipv6 address is missing closing '\]'/;
    var noOpenBracket = /^'\]' present without '\['/;
    var noColonPrePort = /^missing colon after '\]' before the port/;
    var badStrings = [
        {s: undefined, r: missingConnString},
        {s: 7, r: incorrectType},
        {s: null, r: incorrectType},
        {s: "", r: emptyConnString},
        {s: "    ", r: emptyConnString},
        {s: ":", r: emptyHost},
        {s: "/", r: badHost},
        {s: ":/", r: emptyHost},
        {s: ":/test", r: emptyHost},
        {s: ":27999/", r: emptyHost},
        {s: ":27999/test", r: emptyHost},
        {s: "/test", r: badHost},
        {s: "localhost:/test", r: noPort},
        {s: "[::1]:/test", r: noPort},
        {s: "[::1]:cat/test", r: badPort},
        {s: "[::1]:1cat/test", r: badPort},
        {s: "[::1]:123456/test", r: invalidPort},
        {s: "[::1]:65536/test", r: invalidPort},
        {s: "127.0.0.1:65536/test", r: invalidPort},
        {s: "::1:27999/test", r: moreThanOneColon},
        {s: "0:0::0:0:1:27999/test", r: moreThanOneColon},
        {s: "0000:0000:0000:0000:0000:0000:0000:0001:27999/test", r: moreThanOneColon},
        {s: "a[127.0.0.1]:27999/", r: charBeforeSquareBracket},
        {s: "a[::1:]27999/", r: charBeforeSquareBracket},
        {s: "[::1:27999/", r: noCloseBracket},
        {s: "[::1:]27999/", r: noColonPrePort},
        {s: "::1]:27999/", r: noOpenBracket},
    ];

    var substitutePort = function(connectionString) {
        // This will be called with non-strings as well as strings, so we need to catch exceptions
        try {
            return connectionString.replace("27999", "" + port);
        } catch (e) {
            return connectionString;
        }
    };

    var testGood = function(i, connectionString) {
        print("\n---\nTesting good connection string " + i + " (\"" + connectionString + "\") ...");
        var gotException = false;
        var exception;
        try {
            var connectDB = connect(connectionString);
            connectDB = null;
        } catch (e) {
            gotException = true;
            exception = e;
        }
        if (!gotException) {
            print("Good connection string " + i + " (\"" + connectionString +
                  "\") correctly validated");
            return;
        }
        var message = "FAILED to correctly validate goodString " + i + " (\"" + connectionString +
            "\"):  exception was \"" + tojson(exception) + "\"";
        doassert(message);
    };

    var testBad = function(i, connectionString, errorRegex) {
        print("\n---\nTesting bad connection string " + i + " (\"" + connectionString + "\") ...");
        var gotException = false;
        var gotCorrectErrorText = false;
        var exception;
        try {
            var connectDB = connect(connectionString);
            connectDB = null;
        } catch (e) {
            gotException = true;
            exception = e;
            if (errorRegex.test(e.message)) {
                gotCorrectErrorText = true;
            }
        }
        if (gotCorrectErrorText) {
            print("Bad connection string " + i + " (\"" + connectionString +
                  "\") correctly rejected:\n" + tojson(exception));
            return;
        }
        var message = "FAILED to generate correct exception for badString " + i + " (\"" +
            connectionString + "\"): ";
        if (gotException) {
            message += "exception was \"" + tojson(exception) + "\", it should have matched \"" +
                errorRegex.toString() + "\"";
        } else {
            message += "no exception was thrown";
        }
        doassert(message);
    };

    var i;
    jsTest.log("TESTING " + goodStrings.length + " good connection strings");
    for (i = 0; i < goodStrings.length; ++i) {
        testGood(i, substitutePort(goodStrings[i]));
    }

    jsTest.log("TESTING " + badStrings.length + " bad connection strings");
    for (i = 0; i < badStrings.length; ++i) {
        testBad(i, substitutePort(badStrings[i].s), badStrings[i].r);
    }

    jsTest.log("SUCCESSFUL test completion");
})();
