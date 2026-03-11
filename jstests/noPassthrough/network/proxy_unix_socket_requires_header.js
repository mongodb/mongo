/**
 * Verifies that connections over the proxy Unix socket are rejected without a PROXY protocol v2
 * header, and accepted when routed through the proxy protocol server that injects the header.
 * Also exercises peer credential validation failure paths (unauthorized and general failure)
 * and checks the corresponding log entries from asio_transport_layer.cpp (11793400, 11793401).
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

// ErrorCodes used by proxyUnixDomainSocketPeerCredentialValidationResult failpoint (code numberInt).
const kUnauthorizedCode = 13;
const kInternalErrorCode = 1;

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

function testWithVersion(conn, ingressPort, egressPort, proxySocketPath, version, shouldSucceed) {
    const proxyServer = new ProxyProtocolServer(ingressPort, egressPort, version, {egressUnixSocket: proxySocketPath});
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
    }
}

function runPeerCredentialValidationFailureTest(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);

    // Unauthorized branch: failpoint returns Unauthorized -> asio_transport_layer logs 11793400.
    const fpUnauthorized = configureFailPoint(conn, "proxyUnixDomainSocketPeerCredentialValidationOverride", {
        mode: "alwaysOn",
        data: {code: kUnauthorizedCode},
    });
    const proxyServerUnauth = new ProxyProtocolServer(allocatePort(), conn.port, 2, {
        egressUnixSocket: proxySocketPath,
    });
    proxyServerUnauth.setTLVs([{type: 0xe0, value: "unix-proxy"}]);
    proxyServerUnauth.start();
    try {
        const uriUnauth = `mongodb://127.0.0.1:${proxyServerUnauth.getIngressPort()}`;
        assert.throws(() => new Mongo(uriUnauth));
        checkLog.containsRelaxedJson(conn, 11793400, {}, 1, 30 * 1000);
    } finally {
        proxyServerUnauth.stop();
        fpUnauthorized.off();
    }

    // General check failure branch: failpoint returns InternalError -> asio_transport_layer logs 11793401.
    const fpInternalError = configureFailPoint(conn, "proxyUnixDomainSocketPeerCredentialValidationOverride", {
        mode: "alwaysOn",
        data: {code: kInternalErrorCode},
    });
    const proxyServerInternal = new ProxyProtocolServer(allocatePort(), conn.port, 2, {
        egressUnixSocket: proxySocketPath,
    });
    proxyServerInternal.setTLVs([{type: 0xe0, value: "unix-proxy"}]);
    proxyServerInternal.start();
    try {
        const uriInternal = `mongodb://127.0.0.1:${proxyServerInternal.getIngressPort()}`;
        assert.throws(() => new Mongo(uriInternal));
        checkLog.containsRelaxedJson(conn, 11793401, {}, 1, 30 * 1000);
    } finally {
        proxyServerInternal.stop();
        fpInternalError.off();
    }
}

function runTest(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);

    // A direct connection to the proxy unix socket should fail since there is no proxy protocol header.
    assertConnectionFails(conn, proxySocketPath);

    // Test with v1 and v2 proxy protocol header. Only V2 should be accepted.
    // Proxy connects to mongod via the Unix socket (egress).
    testWithVersion(conn, allocatePort(), conn.port, proxySocketPath, 1, false /* shouldSucceed */);
    testWithVersion(conn, allocatePort(), conn.port, proxySocketPath, 2, true /* shouldSucceed */);

    // Peer credential validation failure paths (logs 11793400 and 11793401 from asio_transport_layer.cpp).
    runPeerCredentialValidationFailureTest(conn, prefix);
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
