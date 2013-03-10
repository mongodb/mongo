// Test validation of connection strings passed to the JavaScript "connect()" function.
// Related to SERVER-8030.

var goodStrings = [
        "localhost:27999/test",
        "127.0.0.1:27999/test"
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
        { s: "127.0.0.1:/test",         r: /^Missing port number/ },
        { s: "127.0.0.1:cat/test",      r: /^Invalid port number/ },
        { s: "127.0.0.1:1cat/test",     r: /^Invalid port number/ },
        { s: "127.0.0.1:123456/test",   r: /^Invalid port number/ },
        { s: "127.0.0.1:65536/test",    r: /^Invalid port number/ },
        { s: "::1:65536/test",          r: /^Invalid port number/ },
        { s: "127.0.0.1:27999/",        r: /^Missing database name/ },
        { s: "::1:27999/",              r: /^Missing database name/ }
    ];

function testGood(i, connectionString) {
    print("\nTesting good connection string " + i + " (\"" + connectionString + "\") ...");
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
}

function testBad(i, connectionString, errorRegex) {
    print("\nTesting bad connection string " + i + " (\"" + connectionString + "\") ...");
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
}

var i;
jsTest.log("TESTING " + goodStrings.length + " good connection strings");
for (i = 0; i < goodStrings.length; ++i) {
    testGood(i, goodStrings[i]);
}

jsTest.log("TESTING " + badStrings.length + " bad connection strings");
for (i = 0; i < badStrings.length; ++i) {
    testBad(i, badStrings[i].s, badStrings[i].r);
}

jsTest.log("SUCCESSFUL test completion");
