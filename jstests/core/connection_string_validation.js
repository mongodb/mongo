// Test validation of connection strings passed to the JavaScript "connect()" function.
// Related to SERVER-8030.

port = "27017";

if (db.getMongo().host.indexOf(":") >= 0) {
    var idx = db.getMongo().host.indexOf(":");
    port = db.getMongo().host.substring(idx + 1);
}

var goodStrings = [
    "localhost:" + port + "/test",
    "127.0.0.1:" + port + "/test",
    "127.0.0.1:" + port + "/",
];

var missingConnString = /^Missing connection string$/;
var incorrectType = /^Incorrect type/;
var emptyConnString = /^Empty connection string$/;
var badHost = /^Failed to parse mongodb/;
var emptyHost = /^Empty host component/;
var noPort = /^No digits/;
var badPort = /^Bad digit/;
var invalidPort = /^Port number \d+ out of range/;
var multipleColon = /^More than one ':' detected./;
var noReplSet = /^connect failed to replica set/;
var badStrings = [
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
    {s: "mongodb://127.0.0.1:cat/test", r: badPort},
    {s: "mongodb://127.0.0.1:1cat/test", r: badPort},
    {s: "mongodb://127.0.0.1:123456/test", r: invalidPort},
    {s: "mongodb://127.0.0.1:65536/test", r: invalidPort},
    {s: "mongodb://::1:65536/test", r: multipleColon},
    {s: "mongodb://::1:" + port + "/", r: multipleColon}
];

function testGoodAsURI(i, uri) {
    uri = "mongodb://" + uri;
    print("\nTesting good uri " + i + " (\"" + uri + "\") ...");
    var gotException = false;
    var exception;
    try {
        var m_uri = MongoURI(uri);
        var connectDB = connect(uri);
        connectDB = null;
    } catch (e) {
        gotException = true;
        exception = e;
    }
    if (!gotException) {
        print("Good uri " + i + " (\"" + uri + "\") correctly validated");
        return;
    }
    var message = "FAILED to correctly validate goodString " + i + " (\"" + uri +
        "\"):  exception was \"" + tojson(exception) + "\"";
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
}

var i;
jsTest.log("TESTING " + goodStrings.length + " good connection strings");
for (i = 0; i < goodStrings.length; ++i) {
    testGoodAsURI(i, goodStrings[i]);
}

jsTest.log("TESTING " + badStrings.length + " bad connection strings");
for (i = 0; i < badStrings.length; ++i) {
    testBad(i, badStrings[i].s, badStrings[i].r);
}

jsTest.log("SUCCESSFUL test completion");
