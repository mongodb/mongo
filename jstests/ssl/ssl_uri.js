// Test that the ssl=true/false option is honored in shell URIs.

var shouldSucceed = function(uri) {
    var conn = new Mongo(uri);
    var res = conn.getDB('admin').runCommand({"hello": 1});
    assert(res.ok);
};

var shouldFail = function(uri) {
    assert.throws(function(uri) {
        new Mongo(uri);
    }, [uri], "network error while attempting to run command");
};

// Start up a mongod with ssl required.
var tlsMongo = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
});

var tlsURI = "mongodb://localhost:" + tlsMongo.port + "/admin";

// When talking to a server with SSL, connecting with ssl=false fails.
shouldSucceed(tlsURI);
shouldSucceed(tlsURI + "?ssl=true");
shouldFail(tlsURI + "?ssl=false");

var connectWithURI = function(uri) {
    return runMongoProgram('mongo',
                           '--tls',
                           '--tlsAllowInvalidCertificates',
                           '--tlsCAFile',
                           'jstests/libs/ca.pem',
                           '--tlsCertificateKeyFile',
                           'jstests/libs/client.pem',
                           uri,
                           '--eval',
                           'db.runCommand({hello: 1})');
};

var shouldConnect = function(uri) {
    assert.eq(connectWithURI(uri), 0, "should have been able to connect with " + uri);
};

var shouldNotConnect = function(uri) {
    assert.eq(connectWithURI(uri), 1, "should not have been able to connect with " + uri);
};

// When talking to a server with SSL, connecting with ssl=false on the command line fails.
shouldConnect(tlsURI);
shouldNotConnect(tlsURI + "?ssl=false");
shouldConnect(tlsURI + "?ssl=true");

// Connecting with ssl=true without --tls will not work
var res = runMongoProgram('mongo', tlsURI + "?ssl=true", '--eval', 'db.runCommand({hello: 1})');
assert.eq(res, 1, "should not have been able to connect without --tls");

// Clean up
MongoRunner.stopMongod(tlsMongo);
