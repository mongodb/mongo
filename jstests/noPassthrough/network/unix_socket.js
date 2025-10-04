/*
 * This test checks that when mongod is started with UNIX sockets enabled or disabled,
 * that we are able to connect (or not connect) and run commands:
 * 1) There should be a default unix socket of /tmp/mongod-portnumber.sock
 * 2) If you specify a custom socket in the bind_ip param, that it shows up as
 *    /tmp/custom_socket.sock
 * 3) That bad socket paths, like paths longer than the maximum size of a sockaddr
 *    cause the server to exit with an error (socket names with whitespace are now supported)
 * 4) That the default unix socket doesn't get created if --nounixsocket is specified
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// @tags: [
//   requires_sharding,
// ]
// This test will only work on POSIX machines.
if (_isWindows()) {
    quit();
}

// Checking index consistency involves reconnecting to the mongos.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckShardFilteringMetadata = true;

// Do not fail if this test leaves unterminated processes because testSockOptions
// is expected to throw before it calls stopMongod.
TestData.ignoreUnterminatedProcesses = true;

// Do not check metadata or UUID consistency as it would require a connection to the mongos and this
// is bound to a specific socket for testing purposes.
TestData.skipCheckMetadataConsistency = true;
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

let doesLogMatchRegex = function (logArray, regex) {
    for (let i = logArray.length - 1; i >= 0; i--) {
        let regexInLine = regex.exec(logArray[i]);
        if (regexInLine != null) {
            return true;
        }
    }
    return false;
};

let checkSocket = function (path) {
    const start = new Date();
    assert(fileExists(path), `${start.toISOString()}: ${path} does not exist`);
    let conn = new Mongo(path);
    assert.commandWorked(conn.getDB("admin").runCommand("ping"), `Expected ping command to succeed for ${path}`);
};

let testSockOptions = function (bindPath, expectSockPath, optDict, bindSep = ",", optMongos) {
    var optDict = optDict || {};
    if (bindPath) {
        optDict["bind_ip"] = `${MongoRunner.dataDir}/${bindPath}${bindSep}127.0.0.1`;
    }

    let conn, shards;
    if (optMongos) {
        shards = new ShardingTest({shards: 1, mongos: 1, other: {mongosOptions: optDict}});
        assert.neq(shards, null, "Expected cluster to start okay");
        conn = shards.s0;
    } else {
        conn = MongoRunner.runMongod(optDict);
    }

    assert.neq(conn, null, `Expected ${optMongos ? "mongos" : "mongod"} to start okay`);

    const defaultUNIXSocket = jsTestOptions().shellGRPC
        ? `/tmp/mongodb-grpc-${conn.port}.sock`
        : `/tmp/mongodb-${conn.port}.sock`;
    let checkPath = defaultUNIXSocket;
    if (expectSockPath) {
        checkPath = `${MongoRunner.dataDir}/${expectSockPath}`;
    }

    checkSocket(checkPath);

    // Test the naming of the unix socket
    // Due to https://github.com/grpc/grpc/issues/35006, gRPC unix sockets are unnamed.
    if (!jsTestOptions().shellGRPC) {
        let log = conn.adminCommand({getLog: "global"});
        assert.commandWorked(log, "Expected getting the log to work");
        let ll = log.log;
        let re = new RegExp("anonymous unix socket");
        assert(doesLogMatchRegex(ll, re), "Log message did not contain 'anonymous unix socket'");
    }

    if (optMongos) {
        shards.stop();
    } else {
        MongoRunner.stopMongod(conn);
    }

    assert.eq(fileExists(checkPath), false);
};

// Check that the default unix sockets work
testSockOptions();
testSockOptions(undefined, undefined, undefined, ",", true);

// Check that a custom unix socket path works
testSockOptions("testsock.socket", "testsock.socket");
testSockOptions("testsock.socket", "testsock.socket", undefined, ",", true);

// Check that a custom unix socket path works with spaces
testSockOptions("test sock.socket", "test sock.socket");
testSockOptions("test sock.socket", "test sock.socket", undefined, ",", true);

// Check that a custom unix socket path works with spaces before the comma and after
testSockOptions("testsock.socket ", "testsock.socket", undefined, ", ");
testSockOptions("testsock.socket ", "testsock.socket", undefined, ", ", true);

// Check that a bad UNIX path breaks
assert.throws(function () {
    let badname = "a".repeat(200) + ".socket";
    testSockOptions(badname, badname);
});

// Check that if UNIX sockets are disabled that we aren't able to connect over UNIX sockets
assert.throws(function () {
    testSockOptions(undefined, undefined, {nounixsocket: ""});
});

// Check the unixSocketPrefix option
let socketPrefix = `${MongoRunner.dataDir}/socketdir`;
mkdir(socketPrefix);

if (jsTestOptions().shellGRPC) {
    var grpcPort = allocatePort();
    let sockName = `socketdir/mongodb-grpc-${grpcPort}.sock`;
    testSockOptions(undefined, sockName, {unixSocketPrefix: socketPrefix, grpcPort: grpcPort});
} else {
    var port = allocatePort();
    let sockName = `socketdir/mongodb-${port}.sock`;
    testSockOptions(undefined, sockName, {unixSocketPrefix: socketPrefix, port: port});
}

if (jsTestOptions().shellGRPC) {
    var grpcPort = allocatePort();
    var sockName = `socketdir/mongodb-grpc-${grpcPort}.sock`;
    testSockOptions(undefined, sockName, {unixSocketPrefix: socketPrefix, grpcPort: grpcPort}, ",", true);
} else {
    var port = allocatePort();
    var sockName = `socketdir/mongodb-${port}.sock`;
    testSockOptions(undefined, sockName, {unixSocketPrefix: socketPrefix, port: port}, ",", true);
}
