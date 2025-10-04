// Test validation of IPv6 connection strings passed to the JavaScript "connect()" function.
// Related to SERVER-8030.

// This file runs in two modes: outer and inner.  This is to enable testing with --ipv6.
// The outer mode test starts a mongod with --ipv6 and then starts a mongo shell with --ipv6
// and a command line to run the test in inner_mode.  The inner mode test is the actual test.
if ("undefined" == typeof inner_mode) {
    // Start a mongod with --ipv6
    jsTest.log("Outer mode test starting mongod with --ipv6");
    // NOTE: bind_ip arg is present to test if it can parse ipv6 addresses (::1 in this case).
    // Unfortunately, having bind_ip = ::1 won't work in the test framework (But does work when
    // tested manually), so 127.0.0.1 is also present so the test mongo shell can connect
    // with that address.
    let mongod = MongoRunner.runMongod({ipv6: "", bind_ip: "::1,127.0.0.1"});
    if (mongod == null) {
        jsTest.log("Unable to run test because ipv6 is not on machine, see BF-10990");
        quit();
    }
    let args = [
        "mongo",
        "--nodb",
        "--ipv6",
        "--host",
        "::1",
        "--port",
        mongod.port,
        "--eval",
        "inner_mode=true;port=" + mongod.port + ";",
        "jstests/noPassthroughWithMongod/network/ipv6_connection_string_validation.js",
    ];
    let exitCode = _runMongoProgram.apply(null, args);
    jsTest.log("Inner mode test finished, exit code was " + exitCode);

    // Pass the inner test's exit code back as the outer test's exit code
    if (exitCode != 0) {
        doassert("inner test failed with exit code " + exitCode);
    }
    MongoRunner.stopMongod(mongod);
    quit();
}

let goodStrings = [
    "localhost:27999/test",
    "[::1]:27999/test",
    "[0:0:0:0:0:0:0:1]:27999/test",
    "[0000:0000:0000:0000:0000:0000:0000:0001]:27999/test",
    "localhost:27999",
    "[::1]:27999",
    "[0:0:0:0:0:0:0:1]:27999",
    "[0000:0000:0000:0000:0000:0000:0000:0001]:27999",
];

let missingConnString = /^Missing connection string$/;
let incorrectType = /^Incorrect type/;
let emptyConnString = /^Empty connection string$/;
let badHost = /^Failed to parse mongodb/;
let emptyHost = /^Empty host component/;
let noPort = /^No digits/;
let invalidPort = /^Port number \d+ out of range/;
let moreThanOneColon = /^More than one ':' detected/;
let charBeforeSquareBracket = /^'\[' present, but not first character/;
let noCloseBracket = /^ipv6 address is missing closing '\]'/;
let noOpenBracket = /^'\]' present without '\['/;
let noColonPrePort = /^missing colon after '\]' before the port/;
let badStrings = [
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
    {s: "[::1]:cat/test", c: ErrorCodes.FailedToParse},
    {s: "[::1]:1cat/test", c: ErrorCodes.FailedToParse},
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

let substitutePort = function (connectionString) {
    // This will be called with non-strings as well as strings, so we need to catch exceptions
    try {
        return connectionString.replace("27999", "" + port);
    } catch (e) {
        return connectionString;
    }
};

let testGood = function (i, connectionString) {
    print("\n---\nTesting good connection string " + i + ' ("' + connectionString + '") ...');
    let gotException = false;
    let exception;
    try {
        let connectDB = connect(connectionString);
        connectDB = null;
    } catch (e) {
        gotException = true;
        exception = e;
    }
    if (!gotException) {
        print("Good connection string " + i + ' ("' + connectionString + '") correctly validated');
        return;
    }
    let message =
        "FAILED to correctly validate goodString " +
        i +
        ' ("' +
        connectionString +
        '"):  exception was "' +
        tojson(exception) +
        '"';
    doassert(message);
};

let testBad = function (i, connectionString, errorRegex, errorCode) {
    print("\n---\nTesting bad connection string " + i + ' ("' + connectionString + '") ...');
    let gotException = false;
    let gotCorrectErrorText = false;
    let gotCorrectErrorCode = false;
    let exception;
    try {
        let connectDB = connect(connectionString);
        connectDB = null;
    } catch (e) {
        gotException = true;
        exception = e;
        if (errorRegex && errorRegex.test(e.message)) {
            gotCorrectErrorText = true;
        }
        if (errorCode == e.code) {
            gotCorrectErrorCode = true;
        }
    }
    if (gotCorrectErrorText || gotCorrectErrorCode) {
        print("Bad connection string " + i + ' ("' + connectionString + '") correctly rejected:\n' + tojson(exception));
        return;
    }
    let message = "FAILED to generate correct exception for badString " + i + ' ("' + connectionString + '"): ';
    if (gotException) {
        message += 'exception was "' + tojson(exception) + '", it should have matched "' + errorRegex.toString() + '"';
    } else {
        message += "no exception was thrown";
    }
    doassert(message);
};

let i;
jsTest.log("TESTING " + goodStrings.length + " good connection strings");
for (i = 0; i < goodStrings.length; ++i) {
    testGood(i, substitutePort(goodStrings[i]));
}

jsTest.log("TESTING " + badStrings.length + " bad connection strings");
for (i = 0; i < badStrings.length; ++i) {
    testBad(i, substitutePort(badStrings[i].s), badStrings[i].r, badStrings[i].c);
}

jsTest.log("SUCCESSFUL test completion");
