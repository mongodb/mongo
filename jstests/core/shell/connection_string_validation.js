// Test validation of connection strings passed to the JavaScript "connect()" function.
// @tags: [
//   uses_multiple_connections,
//   docker_incompatible,
//   # network_error_and_txn_override.js will timeout with assert.soon and
//   # give a different error from what test expects.
//   does_not_support_stepdowns,
// ]
// Related to SERVER-8030.

let port = "27017";

if (db.getMongo().host.indexOf(":") >= 0) {
    let idx = db.getMongo().host.indexOf(":");
    port = db.getMongo().host.substring(idx + 1);
}

let goodStrings = ["localhost:" + port + "/test", "127.0.0.1:" + port + "/test", "127.0.0.1:" + port + "/"];

let missingConnString = /^Missing connection string$/;
let incorrectType = /^Incorrect type/;
let emptyConnString = /^Empty connection string$/;
let badHost = /^Failed to parse mongodb/;
let emptyHost = /^Empty host component/;
let noPort = /^No digits/;
let invalidPort = /^Port number \d+ out of range/;
let multipleColon = /^More than one ':' detected./;
let noReplSet = /^connect failed to replica set/;
let badStrings = [
    {s: undefined, r: missingConnString},
    {s: 7, r: incorrectType},
    {s: null, r: incorrectType},
    {s: "", r: emptyConnString},
    {s: "    ", r: emptyConnString},
    {s: ":", r: emptyHost},
    {s: "/", r: badHost},
    {s: "/test", r: badHost},
    {s: ":/", r: emptyHost},
    {s: ":/test", r: emptyHost},
    {s: "mongodb://:" + port + "/", r: emptyHost},
    {s: "mongodb://:" + port + "/test", r: emptyHost},
    {s: "mongodb://localhost:/test", r: noPort},
    {s: "mongodb://127.0.0.1:/test", r: noPort},
    {s: "mongodb://127.0.0.1:cat/test", c: ErrorCodes.FailedToParse},
    {s: "mongodb://127.0.0.1:1cat/test", c: ErrorCodes.FailedToParse},
    {s: "mongodb://127.0.0.1:123456/test", r: invalidPort},
    {s: "mongodb://127.0.0.1:65536/test", r: invalidPort},
    {s: "mongodb://::1:65536/test", r: multipleColon},
    {s: "mongodb://::1:" + port + "/", r: multipleColon},
];

function testGoodAsURI(i, uri) {
    uri = "mongodb://" + uri;
    print("\nTesting good uri " + i + ' ("' + uri + '") ...');
    let gotException = false;
    let exception;
    try {
        let m_uri = MongoURI(uri);
        let connectDB = connect(uri);
        connectDB = null;
    } catch (e) {
        gotException = true;
        exception = e;
    }
    if (!gotException) {
        print("Good uri " + i + ' ("' + uri + '") correctly validated');
        return;
    }
    let message =
        "FAILED to correctly validate goodString " + i + ' ("' + uri + '"):  exception was "' + tojson(exception) + '"';
    doassert(message);
}

function testBad(i, connectionString, errorRegex, errorCode) {
    print("\nTesting bad connection string " + i + ' ("' + connectionString + '") ...');
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
}

let i;
jsTest.log("TESTING " + goodStrings.length + " good connection strings");
for (i = 0; i < goodStrings.length; ++i) {
    testGoodAsURI(i, goodStrings[i]);
}

jsTest.log("TESTING " + badStrings.length + " bad connection strings");
for (i = 0; i < badStrings.length; ++i) {
    testBad(i, badStrings[i].s, badStrings[i].r, badStrings[i].c);
}

jsTest.log("SUCCESSFUL test completion");
