/**
 * Verifies that connections over the proxy Unix socket are rejected without a PROXY protocol v2
 * header, and accepted when routed through the proxy protocol server that injects the header.
 * Also exercises peer credential validation paths.
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

const kInternalErrorCode = 1;

function makeProxySocketPath(prefix, port) {
    return `${prefix}/proxy-mongodb-${port}.sock`;
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

function runPeerCredentialValidationTest(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);

    const proxyServer = new ProxyProtocolServer(allocatePort(), conn.port, 2, {
        egressUnixSocket: proxySocketPath,
    });
    proxyServer.setTLVs([{type: 0xe0, value: "unix-proxy"}]);
    proxyServer.start();

    try {
        {
            // Proxy server GID of 1000 should succeed.
            const fp = configureFailPoint(conn, "proxyUnixDomainSocketPeerCredentialValidationOverride", {
                mode: "alwaysOn",
                data: {expectedGid: NumberInt(1000), remoteGid: NumberInt(1000)},
            });
            let uri = `mongodb://127.0.0.1:${proxyServer.getIngressPort()}`;
            new Mongo(uri);
            fp.off();
        }

        {
            // Proxy server GID of 1001 should fail when 1000 is expected.
            const fp = configureFailPoint(conn, "proxyUnixDomainSocketPeerCredentialValidationOverride", {
                mode: "alwaysOn",
                data: {expectedGid: NumberInt(1000), remoteGid: NumberInt(1001)},
            });
            let uri = `mongodb://127.0.0.1:${proxyServer.getIngressPort()}`;
            assert.throws(() => new Mongo(uri));
            checkLog.containsRelaxedJson(conn, 11793400, {}, 1, 30 * 1000);

            // If proxyUnixSocketCheckPermissions is disabled, connection succeedes.
            assert.commandWorked(
                conn.adminCommand({
                    setParameter: 1,
                    proxyUnixSocketCheckPermissions: false,
                }),
            );
            new Mongo(uri);

            // Set parameter back to on for next tests.
            assert.commandWorked(
                conn.adminCommand({
                    setParameter: 1,
                    proxyUnixSocketCheckPermissions: true,
                }),
            );
            fp.off();
        }

        {
            // Test failure log if server is unable to validate.
            const fp = configureFailPoint(conn, "proxyUnixDomainSocketPeerCredentialValidationOverride", {
                mode: "alwaysOn",
                data: {code: kInternalErrorCode},
            });
            let uri = `mongodb://127.0.0.1:${proxyServer.getIngressPort()}`;
            assert.throws(() => new Mongo(uri));
            checkLog.containsRelaxedJson(conn, 11793401, {}, 1, 30 * 1000);
            fp.off();
        }
    } finally {
        proxyServer.stop();
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

    // Validate proxyUnixSocketCheckPermissions parameter functionality.
    runPeerCredentialValidationTest(conn, prefix);
}

function runMongodTest() {
    const prefix = `${MongoRunner.dataPath}${jsTestName()}_mongod`;
    mkdir(prefix);

    const mongod = MongoRunner.runMongod({
        proxyUnixSocketPrefix: prefix,
        setParameter: {
            proxyProtocolTimeoutSecs: 1,
            logComponentVerbosity: {network: {verbosity: 4}},
        },
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
                setParameter: {
                    proxyProtocolTimeoutSecs: 1,
                    logComponentVerbosity: {network: {verbosity: 4}},
                },
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
