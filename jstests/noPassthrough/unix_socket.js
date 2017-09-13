/*
 * This test checks that when mongod is started with UNIX sockets enabled or disabled,
 * that we are able to connect (or not connect) and run commands:
 * 1) There should be a default unix socket of /tmp/mongod-portnumber.sock
 * 2) If you specify a custom socket in the bind_ip param, that it shows up as
 *    /tmp/custom_socket.sock
 * 3) That bad socket paths, like paths longer than the maximum size of a sockaddr
 *    cause the server to exit with an error
 * 4) That the default unix socket doesn't get created if --nounixsocket is specified
 */
(function() {
    'use strict';
    // This test will only work on POSIX machines.
    if (_isWindows()) {
        return;
    }

    var doesLogMatchRegex = function(logArray, regex) {
        for (let i = (logArray.length - 1); i >= 0; i--) {
            var regexInLine = regex.exec(logArray[i]);
            if (regexInLine != null) {
                return true;
            }
        }
        return false;
    };

    var checkSocket = function(path) {
        var conn = new Mongo(path);
        assert.commandWorked(conn.getDB("admin").runCommand("ping"),
                             `Expected ping command to succeed for ${path}`);
    };

    var testSockOptions = function(bindPath, expectSockPath, optDict) {
        var optDict = optDict || {};
        if (bindPath) {
            optDict["bind_ip"] = `${MongoRunner.dataDir}/${bindPath},127.0.0.1`;
        }
        var conn = MongoRunner.runMongod(optDict);
        assert.neq(conn, null, "Expected mongod to start okay");

        const defaultUNIXSocket = `/tmp/mongodb-${conn.port}.sock`;
        var checkPath = defaultUNIXSocket;
        if (expectSockPath) {
            checkPath = `${MongoRunner.dataDir}/${expectSockPath}`;
        }

        checkSocket(checkPath);

        // Test the naming of the unix socket
        var log = conn.adminCommand({getLog: 'global'});
        assert.commandWorked(log, "Expected getting the log to work");
        var ll = log.log;
        var re = new RegExp("anonymous unix socket");
        assert(doesLogMatchRegex(ll, re), "Log message did not contain 'anonymous unix socket'");

        MongoRunner.stopMongod(conn);
    };

    // Check that the default unix sockets work
    testSockOptions();

    // Check that a custom unix socket path works
    testSockOptions("testsock.socket", "testsock.socket");

    // Check that a bad UNIX path breaks
    assert.throws(function() {
        var badname = "a".repeat(200) + ".socket";
        testSockOptions(badname, badname);
    });

    // Check that if UNIX sockets are disabled that we aren't able to connect over UNIX sockets
    assert.throws(function() {
        testSockOptions(undefined, undefined, {nounixsocket: ""});
    });

    // Check the unixSocketPrefix option
    var socketPrefix = `${MongoRunner.dataDir}/socketdir`;
    mkdir(socketPrefix);
    var port = allocatePort();
    testSockOptions(
        undefined, `socketdir/mongodb-${port}.sock`, {unixSocketPrefix: socketPrefix, port: port});

})();
