/**
 * Test the proxyUnixSocketMaximumHeaderSize server parameter.
 * This parameter controls the maximum size of the proxy protocol header
 * that can be read from proxy Unix domain sockets.
 *
 * @tags: [
 *   grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    // Unix domain sockets are not supported on Windows
    quit();
}

import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function makeProxySocketPath(prefix, port) {
    return `${prefix}/proxy-mongodb-${port}.sock`;
}

const kDefaultHeaderSize = 536;
const kDefaultProxyUnixSocketHeaderSize = 4096;
const kMaxConfigurableProxyUnixSocketHeaderSize = 16384;
const kProxyIngressPort = allocatePort();
const kProxyEgressPort = allocatePort();
const kProxyVersion = 2;
const uri = `mongodb://127.0.0.1:${kProxyIngressPort}`;

let currentProxyProtocolSize = 0;
let currentProxyServer = null;

function setParameterWithAssert(conn, kvp, shouldSucceed = true) {
    const command = {setParameter: 1, ...kvp};
    const result = conn.adminCommand(command);

    const assertion = shouldSucceed ? assert.commandWorked : assert.commandFailed;
    assertion(result, `Expected setParameter for ${kvp} ${shouldSucceed ? "to succeed" : "to fail"}`);
}

function getParameter(conn, param) {
    const command = {getParameter: 1, [param]: 1};
    return assert.commandWorked(conn.adminCommand(command))[param];
}

function setParameterTest(conn) {
    const originalValue = getParameter(conn, "proxyUnixSocketMaximumHeaderSize");

    // proxyUnixSocketMaximumHeaderSize should be larger than kDefaultHeaderSize.
    setParameterWithAssert(conn, {proxyUnixSocketMaximumHeaderSize: kDefaultHeaderSize - 1}, false);

    // proxyUnixSocketMaximumHeaderSize can be btwn kDefaultHeaderSize and kMaxConfigurableProxyUnixSocketHeaderSize.
    setParameterWithAssert(conn, {
        proxyUnixSocketMaximumHeaderSize: (kMaxConfigurableProxyUnixSocketHeaderSize + kDefaultHeaderSize) / 2,
    });

    // proxyUnixSocketMaximumHeaderSize should be smaller than kMaxConfigurableProxyUnixSocketHeaderSize.
    setParameterWithAssert(
        conn,
        {proxyUnixSocketMaximumHeaderSize: kMaxConfigurableProxyUnixSocketHeaderSize + 1},
        false,
    );

    // Restore parameter to original value.
    setParameterWithAssert(conn, {proxyUnixSocketMaximumHeaderSize: originalValue});
}

function assertConnectWithProxyProtocolHeader(size, shouldSucceed) {
    // Update proxy protocol size to use for connection if needed.
    if (size !== currentProxyProtocolSize) {
        setProxyProtocolSize(size);
    }

    const assertion = shouldSucceed ? assert.doesNotThrow : assert.throws;
    assertion(
        () => new Mongo(uri),
        [],
        `Expected connection with proxy protocol size of ${size} ${shouldSucceed ? "to succeed" : "to throw"}`,
    );
}

// Helper function to set the size of the proxy protocol emitted by the current proxy.
function setProxyProtocolSize(size) {
    // Calculate the size of the TLV object based on the required total size.
    // Breakdown of proxy protocol v2 size components:
    //  - 16 bytes: header
    //  - 12 bytes: IPv4 addresses
    //  - 3 bytes: TLV type and length
    const tlvSize = size - 16 - 12 - 3;
    assert(tlvSize);
    const valueStr = "a".repeat(tlvSize);
    currentProxyServer.setTLVs([{"type": 0xe0, "value": valueStr}]);
}

function runConnectTest(conn, prefix) {
    setParameterWithAssert(conn, {logComponentVerbosity: {network: {verbosity: 4}}});
    setParameterWithAssert(conn, {proxyProtocolTimeoutSecs: 1});

    // Phase 1: Proxy with TCP egress to the TCP proxy port — connection is not on proxy unix socket, so default limit applies.
    currentProxyServer = new ProxyProtocolServer(kProxyIngressPort, kProxyEgressPort, kProxyVersion);
    currentProxyServer.start();
    try {
        // When not using the proxy unix socket, we can only use a header up to kDefaultHeaderSize.
        assertConnectWithProxyProtocolHeader(kDefaultHeaderSize, true);
        // Proxy protocol size larger than kDefaultHeaderSize should fail.
        assertConnectWithProxyProtocolHeader(kDefaultHeaderSize + 1, false);
    } finally {
        currentProxyServer.stop();
    }

    // Phase 2: Proxy with real unix socket egress — connection is on proxy unix socket.
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);
    currentProxyServer = new ProxyProtocolServer(kProxyIngressPort, kProxyEgressPort, kProxyVersion, {
        egressUnixSocket: proxySocketPath,
    });
    currentProxyServer.start();
    try {
        // Now we can go past kDefaultHeaderSize.
        assertConnectWithProxyProtocolHeader(kDefaultHeaderSize + 1, true);

        // On the proxy unix socket, the largest we can read by default is kDefaultProxyUnixSocketHeaderSize.
        assertConnectWithProxyProtocolHeader(kDefaultProxyUnixSocketHeaderSize, true);

        // Proxy protocol size larger than kDefaultProxyUnixSocketHeaderSize should fail.
        assertConnectWithProxyProtocolHeader(kDefaultProxyUnixSocketHeaderSize + 1, false);

        // Test that we can configure the proxy unix socket max header size and have the previous header succeed.
        setParameterWithAssert(conn, {proxyUnixSocketMaximumHeaderSize: kDefaultProxyUnixSocketHeaderSize + 1000});
        assertConnectWithProxyProtocolHeader(kDefaultProxyUnixSocketHeaderSize + 1, true);
        assertConnectWithProxyProtocolHeader(kDefaultProxyUnixSocketHeaderSize + 1000, true);

        // Going past the configured max should fail.
        setProxyProtocolSize(kDefaultProxyUnixSocketHeaderSize + 2000);
        assertConnectWithProxyProtocolHeader(kDefaultProxyUnixSocketHeaderSize + 2000, false);
    } finally {
        currentProxyServer.stop();
    }
}

function runTestSuite(conn, prefix) {
    setParameterTest(conn);
    runConnectTest(conn, prefix);
}

{
    const prefix = `${MongoRunner.dataPath}${jsTestName()}_mongod`;
    mkdir(prefix);
    const mongod = MongoRunner.runMongod({
        proxyUnixSocketPrefix: prefix,
        proxyPort: kProxyEgressPort,
    });
    runTestSuite(mongod, prefix);
    MongoRunner.stopMongod(mongod);
}

{
    const prefix = `${MongoRunner.dataPath}${jsTestName()}_mongos`;
    mkdir(prefix);
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            mongosOptions: {
                proxyUnixSocketPrefix: prefix,
                setParameter: {loadBalancerPort: kProxyEgressPort},
            },
        },
    });
    runTestSuite(st.s, prefix);
    st.stop();
}
