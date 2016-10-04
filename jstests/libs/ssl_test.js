// The SSLTest class is used to check if a shell with a certain SSL configuration
// can be used to connect to a server with a given SSL configuration.
// This is necessary because SSL settings are currently process global - so if the mongo shell
// started by resmoke.py has an SSL configuration that's incompatible with a server created with
// MongoRunner, it will not be able to connect to it.

/**
 * A utility for checking if a shell configured with the specified command line options can
 * connect to a server with the specified command line options.
 *
 * The 'serverOpts' and 'clientOpts' objects are in the form of
 * {'cmdLineParam': 'value', ...}. For flag arguments, the empty string is used as the value.
 *
 * For serverOpts a few defaults are set if values are not provided: specifically 'sslMode'
 * (preferSSL), sslPEMKeyFile ("jstests/libs/server.pem"), and sslCAFile
 * "jstests/libs/ca.pem").
 */
function SSLTest(serverOpts, clientOpts) {
    var canonicalServerOpts = function(userProvidedOpts) {
        var canonical = Object.extend({}, userProvidedOpts || {});

        if (!canonical.hasOwnProperty("sslMode")) {
            canonical.sslMode = "preferSSL";
        } else if (canonical.sslMode === "disabled") {
            // should not add further options if SSL is disabled
            return canonical;
        }

        if (!canonical.hasOwnProperty("sslPEMKeyFile")) {
            canonical.sslPEMKeyFile = "jstests/libs/server.pem";
        }
        if (!canonical.hasOwnProperty("sslCAFile")) {
            canonical.sslCAFile = "jstests/libs/ca.pem";
        }
        return canonical;
    };

    this.serverOpts = MongoRunner.mongodOptions(canonicalServerOpts(serverOpts));
    this.port = this.serverOpts.port;
    resetDbpath(this.serverOpts.dbpath);

    this.clientOpts = Object.extend({}, clientOpts || this.defaultSSLClientOptions);
    this.clientOpts.port = this.port;
}

/**
 * The default shell arguments for a shell with SSL enabled.
 */
SSLTest.prototype.defaultSSLClientOptions = {
    "ssl": "",
    "sslPEMKeyFile": "jstests/libs/client.pem",
    "sslAllowInvalidCertificates": "",
    "eval": ";"  // prevent the shell from entering interactive mode
};

/**
 * The default shell arguments for a shell without SSL enabled.
 */
SSLTest.prototype.noSSLClientOptions = {
    eval: ";"  // prevent the shell from entering interactive mode
};

/**
 * Starts a server with the parameters passed to the fixture constructor and then attempts to
 * connect with a shell created with the configured options. Returns whether a connection
 * was successfully established.
 */
SSLTest.prototype.connectWorked = function() {
    var connectTimeoutMillis = 30000;

    var serverArgv = MongoRunner.arrOptions("mongod", this.serverOpts);
    var clientArgv = MongoRunner.arrOptions("mongo", this.clientOpts);

    var serverPID = _startMongoProgram.apply(null, serverArgv);
    try {
        assert.soon(function() {
            return checkProgram(serverPID) && (0 === _runMongoProgram.apply(null, clientArgv));
        }, "connect failed", connectTimeoutMillis);
    } catch (ex) {
        return false;
    } finally {
        _stopMongoProgram(this.port);
    }
    return true;
};
