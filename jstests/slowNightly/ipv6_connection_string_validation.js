// Test validation of IPv6 connection strings passed to the JavaScript "connect()" function.
// Related to SERVER-8030.

// This file runs in two modes: outer and inner.  This is to enable testing with --ipv6.
// The outer mode test starts a mongod with --ipv6 and then starts a mongo shell with --ipv6
// and a command line to run the test in inner_mode.  The inner mode test is the actual test.

if ("undefined" == typeof inner_mode) {

    // Start a mongod with --ipv6
    port = allocatePorts( 1 )[ 0 ];
    var baseName = "jstests_slowNightly_ipv6_connection_string_validation";
    jsTest.log("Outer mode test starting mongod with --ipv6");
    var mongod = startMongod( "--port", port, "--ipv6", "--dbpath", "/data/db/" + baseName );
    var args = [ "mongo", "--nodb", "--ipv6",
                 "--eval", "inner_mode=true;port=" + port + ";",
                 "jstests/slowNightly/ipv6_connection_string_validation.js" ];

    // Start another shell running this test in "inner" mode
    jsTest.log("Outer mode test starting inner mode test");
    var exitCode = _runMongoProgram.apply(null, args);
    jsTest.log("Inner mode test finished, exit code was " + exitCode);

    // Stop the server we started
    jsTest.log("Outer mode test stopping server");
    stopMongod(port, 15);

    // Pass the inner test's exit code back as the outer test's exit code
    quit(exitCode);
}

var goodStrings = [
        "localhost:27999/test",
        "::1:27999/test",
        "0:0:0:0:0:0:0:1:27999/test",
        "0000:0000:0000:0000:0000:0000:0000:0001:27999/test"
];

var badStrings = [
        { s: undefined,                 r: /^Missing connection string$/ },
        { s: 7,                         r: /^Incorrect type/ },
        { s: null,                      r: /^Incorrect type/ },
        { s: "",                        r: /^Empty connection string$/ },
        { s: "    ",                    r: /^Empty connection string$/ },
        { s: ":",                       r: /^Missing host name/ },
        { s: "/",                       r: /^Missing host name/ },
        { s: ":/",                      r: /^Missing host name/ },
        { s: ":/test",                  r: /^Missing host name/ },
        { s: ":27999/",                 r: /^Missing host name/ },
        { s: ":27999/test",             r: /^Missing host name/ },
        { s: "/test",                   r: /^Missing host name/ },
        { s: "localhost:/test",         r: /^Missing port number/ },
        { s: "::1:/test",               r: /^Missing port number/ },
        { s: "::1:cat/test",            r: /^Invalid port number/ },
        { s: "::1:1cat/test",           r: /^Invalid port number/ },
        { s: "::1:123456/test",         r: /^Invalid port number/ },
        { s: "::1:65536/test",          r: /^Invalid port number/ },
        { s: "127.0.0.1:65536/test",    r: /^Invalid port number/ },
        { s: "::1:27999/",              r: /^Missing database name/ },
        { s: "127.0.0.1:27999/",        r: /^Missing database name/ }
];

var substitutePort = function(connectionString) {
    // This will be called with non-strings as well as strings, so we need to catch exceptions
    try {
        return connectionString.replace("27999", "" + port);
    }
    catch (e) {
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
    }
    catch (e) {
        gotException = true;
        exception = e;
    }
    if (!gotException) {
        print("Good connection string " + i +
              " (\"" + connectionString + "\") correctly validated");
        return;
    }
    var message = "FAILED to correctly validate goodString " + i +
                  " (\"" + connectionString + "\"):  exception was \"" + tojson(exception) + "\"";
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
    }
    catch (e) {
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
    var message = "FAILED to generate correct exception for badString " + i +
                  " (\"" + connectionString + "\"): ";
    if (gotException) {
        message += "exception was \"" + tojson(exception) +
                    "\", it should have matched \"" + errorRegex.toString() + "\"";
    }
    else {
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
