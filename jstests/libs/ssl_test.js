// The TLSTest class is used to check if a shell with a certain TLS configuration
// can be used to connect to a server with a given TLS configuration.
// This is necessary because TLS settings are currently process global - so if the mongo shell
// started by resmoke.py has an TLS configuration that's incompatible with a server created with
// MongoRunner, it will not be able to connect to it.

/**
 * A utility for checking if a shell configured with the specified command line options can
 * connect to a server with the specified command line options.
 *
 * The 'serverOpts' and 'clientOpts' objects are in the form of
 * {'cmdLineParam': 'value', ...}. For flag arguments, the empty string is used as the value.
 *
 * For serverOpts a few defaults are set if values are not provided: specifically 'tlsMode'
 * (preferTLS), tlsCertificateKeyFile ("jstests/libs/server.pem"), and tlsCAFile
 * "jstests/libs/ca.pem").
 */
export function TLSTest(serverOpts, clientOpts) {
    var canonicalServerOpts = function(userProvidedOpts) {
        var canonical = Object.extend({}, userProvidedOpts || {});

        if (!canonical.hasOwnProperty("tlsMode")) {
            canonical.tlsMode = "preferTLS";
        } else if (canonical.tlsMode === "disabled") {
            // should not add further options if TLS is disabled
            return canonical;
        }

        if (!canonical.hasOwnProperty("tlsCertificateKeyFile")) {
            canonical.tlsCertificateKeyFile = "jstests/libs/server.pem";
        }
        if (!canonical.hasOwnProperty("tlsCAFile")) {
            canonical.tlsCAFile = "jstests/libs/ca.pem";
        }
        return canonical;
    };

    this.serverOpts = MongoRunner.mongodOptions(canonicalServerOpts(serverOpts));
    this.port = this.serverOpts.port;
    resetDbpath(this.serverOpts.dbpath);

    this.clientOpts = Object.extend({}, clientOpts || this.defaultTLSClientOptions);
    this.clientOpts.port = this.port;
}

/**
 * The default shell arguments for a shell with TLS enabled.
 */
TLSTest.prototype.defaultTLSClientOptions = {
    "tls": "",
    "tlsCertificateKeyFile": "jstests/libs/client.pem",
    "tlsCAFile": "jstests/libs/ca.pem",
    "eval": ";"  // prevent the shell from entering interactive mode
};

/**
 * The default shell arguments for a shell without TLS enabled.
 */
TLSTest.prototype.noTLSClientOptions = {
    eval: ";"  // prevent the shell from entering interactive mode
};

/**
 * Starts a server with the parameters passed to the fixture constructor and then attempts to
 * connect with a shell created with the configured options. Returns whether a connection
 * was successfully established.
 */
TLSTest.prototype.connectWorked = function() {
    var connectTimeoutMillis = 3 * 60 * 1000;

    var serverArgv = MongoRunner.arrOptions("mongod", this.serverOpts);
    var clientArgv = MongoRunner.arrOptions("mongo", this.clientOpts);

    var serverPID = _startMongoProgram.apply(null, serverArgv);
    try {
        // Don't run the hang analyzer because we don't expect connectWorked() to always succeed.
        assert.soon(function() {
            return checkProgram(serverPID).alive &&
                (0 === _runMongoProgram.apply(null, clientArgv));
        }, "connect failed", connectTimeoutMillis, undefined, {runHangAnalyzer: false});
    } catch (ex) {
        return false;
    } finally {
        _stopMongoProgram(this.port);
    }
    return true;
};

/**
 * Starts a server with the parameters passed to the fixture constructor and then attempts to
 * connect with a shell created with the configured options. Returns immediately with true
 * if a connection cannot be established using the configured client options.
 */
TLSTest.prototype.connectFails = function() {
    const connectTimeoutMillis = 3 * 60 * 1000;

    let waitForConnectClientOpts = this.noTLSClientOptions;
    if (this.serverOpts.tlsMode === "requireTLS") {
        waitForConnectClientOpts = this.defaultTLSClientOptions;
    }
    waitForConnectClientOpts.port = this.port;

    const serverArgv = MongoRunner.arrOptions("mongod", this.serverOpts);
    const failingClientArgv = MongoRunner.arrOptions("mongo", this.clientOpts);
    const workingClientArgv = MongoRunner.arrOptions("mongo", waitForConnectClientOpts);
    const serverPID = _startMongoProgram.apply(null, serverArgv);

    // Wait until we can connect to mongod using the working client args
    assert.soon(function() {
        return checkProgram(serverPID).alive &&
            (0 === _runMongoProgram.apply(null, workingClientArgv));
    }, "connect failed", connectTimeoutMillis);
    const result = _runMongoProgram.apply(null, failingClientArgv);
    _stopMongoProgram(this.port);
    return result !== 0;
};