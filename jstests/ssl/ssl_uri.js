// Test that the ssl=true/false option is honored in shell URIs.

(function() {
    "use strict";

    var shouldSucceed = function(uri) {
        var conn = new Merizo(uri);
        var res = conn.getDB('admin').runCommand({"ismaster": 1});
        assert(res.ok);
    };

    var shouldFail = function(uri) {
        assert.throws(function(uri) {
            var conn = new Merizo(uri);
        }, [uri], "network error while attempting to run command");
    };

    // Start up a merizod with ssl required.
    var sslMerizo = MerizoRunner.runMerizod({
        sslMode: "requireSSL",
        sslPEMKeyFile: "jstests/libs/server.pem",
        sslCAFile: "jstests/libs/ca.pem",
    });

    var sslURI = "merizodb://localhost:" + sslMerizo.port + "/admin";

    // When talking to a server with SSL, connecting with ssl=false fails.
    shouldSucceed(sslURI);
    shouldSucceed(sslURI + "?ssl=true");
    shouldFail(sslURI + "?ssl=false");

    var connectWithURI = function(uri) {
        return runMerizoProgram('./merizo',
                               '--ssl',
                               '--sslAllowInvalidCertificates',
                               '--sslCAFile',
                               'jstests/libs/ca.pem',
                               '--sslPEMKeyFile',
                               'jstests/libs/client.pem',
                               uri,
                               '--eval',
                               'db.runCommand({ismaster: 1})');
    };

    var shouldConnect = function(uri) {
        assert.eq(connectWithURI(uri), 0, "should have been able to connect with " + uri);
    };

    var shouldNotConnect = function(uri) {
        assert.eq(connectWithURI(uri), 1, "should not have been able to connect with " + uri);
    };

    // When talking to a server with SSL, connecting with ssl=false on the command line fails.
    shouldConnect(sslURI);
    shouldNotConnect(sslURI + "?ssl=false");
    shouldConnect(sslURI + "?ssl=true");

    // Connecting with ssl=true without --ssl will not work
    var res =
        runMerizoProgram('./merizo', sslURI + "?ssl=true", '--eval', 'db.runCommand({ismaster: 1})');
    assert.eq(res, 1, "should not have been able to connect without --ssl");

    // Clean up
    MerizoRunner.stopMerizod(sslMerizo);
}());
