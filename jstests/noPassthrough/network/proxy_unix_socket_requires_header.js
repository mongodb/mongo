/**
 * Verifies that connections over the proxy Unix socket are rejected without a PROXY protocol v2
 * header, and accepted when routed through the proxy protocol server that injects the header.
 *
 * @tags: [
 *   grpc_incompatible,
 *   requires_sharding,
 * ]
 */

if (_isWindows()) {
    quit();
}

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function makeProxySocketPath(prefix, port) {
    return `${prefix}/unix-mongodb-${port}.sock`;
}

function assertConnectionFails(conn, path) {
    assert.throws(() => new Mongo(path), [], `Expected direct connection to proxy socket to fail: ${path}`);

    assert(
        checkLog.checkContainsOnceJsonStringMatch(conn, 6067900, "msg", "Error while parsing proxy protocol header"),
        "Expected connection to fail because the PROXY protocol header was missing",
    );
}

function testWithVersion(conn, ingressPort, egressPort, version, shouldSucceed) {
    // The proxy server doesn't support making a connection to a unix socket so use this failpoint instead.
    const fp = configureFailPoint(conn, "isConnectedToProxyUnixSocketOverride");

    const proxyServer = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxyServer.setTLVs([{"type": 0xe0, "value": "unix-proxy"}]);
    proxyServer.start();
    try {
        const uri = `mongodb://127.0.0.1:${ingressPort}`;
        if (shouldSucceed) {
            const proxiedConn = new Mongo(uri);
            assert.commandWorked(proxiedConn.getDB("admin").runCommand({ping: 1}));
        } else {
            assertConnectionFails(conn, uri);
        }
    } finally {
        proxyServer.stop();
        fp.off();
    }
}

function runTest(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);

    // A direct connection to the proxy unix socket should fail since there is no proxy protocol header.
    assertConnectionFails(conn, proxySocketPath);

    // Test with v1 and v2 proxy protcol header. Only V2 should be accepted.
    testWithVersion(conn, allocatePort(), conn.port, 1, false /* shouldSucceed */);
    testWithVersion(conn, allocatePort(), conn.port, 2, true /* shouldSucceed */);
}

function runMongodTest() {
    const prefix = `${MongoRunner.dataPath}${jsTestName()}_mongod`;
    mkdir(prefix);

    const mongod = MongoRunner.runMongod({
        proxyUnixSocketPrefix: prefix,
        setParameter: {proxyProtocolTimeoutSecs: 1, logComponentVerbosity: {network: {verbosity: 4}}},
    });

    try {
        runTest(mongod, prefix);
    } finally {
        MongoRunner.stopMongod(mongod);
    }
}

function runMongosTest() {
    const prefix = `${MongoRunner.dataPath}${jsTestName()}_mongos`;
    mkdir(prefix);

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            mongosOptions: {
                proxyUnixSocketPrefix: prefix,
                setParameter: {proxyProtocolTimeoutSecs: 1, logComponentVerbosity: {network: {verbosity: 4}}},
            },
        },
    });

    try {
        runTest(st.s0, prefix);
    } finally {
        st.stop();
    }
}

runMongodTest();
runMongosTest();
