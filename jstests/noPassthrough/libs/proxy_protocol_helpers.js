/**
 * Helpers for testing the proxy protocol.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

export const connectAndHello = (port, isRouter) => {
    jsTestLog(`Attempting to connect to port ${port}`);
    const connStart = Date.now();
    const conn = new Mongo(`mongodb://127.0.0.1:${port}${isRouter ? "/?loadBalanced=true" : ""}`);
    assert.neq(null, conn, `Client was unable to connect to port ${port}`);
    assert.lt(Date.now() - connStart, 10 * 1000, "Client was unable to connect within 10 seconds");
    assert.commandWorked(conn.getDB("admin").runCommand({hello: 1}));
};

export const timeoutEmptyConnection = (ingressPort, egressPort, isRouter) => {
    // Use the connection to set a lower proxy header timeout and validate that empty connections
    // timeout.
    const conn = new Mongo(`mongodb://127.0.0.1:${ingressPort}${isRouter ? "/?loadBalanced=true" : ""}`);
    const previousParameter = conn.adminCommand({getParameter: 1, proxyProtocolTimeoutSecs: 1});
    conn.adminCommand({setParameter: 1, proxyProtocolTimeoutSecs: 1});

    // runProgram blocks until the program is complete. nc should be finished when the server times
    // out the connection that doesn't send data after 1 second, otherwise the test will hang.
    assert.eq(0, runProgram("bash", "-c", `cat </dev/tcp/127.0.0.1/${egressPort}`));

    conn.adminCommand({setParameter: 1, proxyProtocolTimeoutSecs: previousParameter.proxyProtocolTimeoutSecs});
};

export const emptyMessageTest = (ingressPort, egressPort, node, isRouter) => {
    jsTestLog("Connect to proxy port without sending data");
    const pid = _startMongoProgram("bash", "-c", `exec cat < /dev/tcp/127.0.0.1/${egressPort}`);

    // Connecting to the proxy port still succeeds within a reasonable time limit.
    connectAndHello(ingressPort, isRouter);

    // Connecting to the default port still succeeds within a reasonable time limit.
    connectAndHello(node.port, isRouter);

    assert(checkProgram(pid).alive);

    // A connection with no data will timeout.
    timeoutEmptyConnection(ingressPort, egressPort, isRouter);

    stopMongoProgramByPid(pid);
};

export const fuzzingTest = (ingressPort, egressPort, node, isRouter) => {
    const numConnections = 10;

    for (let i = 0; i < numConnections; i++) {
        jsTestLog("Sending random data to proxy port");
        const pid = _startMongoProgram(
            "bash",
            "-c",
            `head -c ${Math.floor(Math.random() * 5000)} /dev/urandom >/dev/tcp/127.0.0.1/${egressPort}`,
        );

        // Connecting to the to the proxy port still succeeds within a reasonable time
        // limit.
        connectAndHello(ingressPort, isRouter);

        // Connecting to the default port still succeeds within a reasonable time limit.
        connectAndHello(node.port, isRouter);

        assert.soon(
            () => !checkProgram(pid).alive,
            "Server should have closed connection with invalid proxy protocol header",
        );
    }
};

export const testProxyProtocolReplicaSet = (ingressPort, egressPort, version, testFn) => {
    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxy_server.start();

    const rs = new ReplSetTest({nodes: 1, nodeOptions: {"proxyPort": egressPort}});
    rs.startSet({
        setParameter: {
            featureFlagMongodProxyProtocolSupport: true,
            "logComponentVerbosity": {network: 5},
        },
    });
    rs.initiate();

    testFn(ingressPort, egressPort, rs.getPrimary(), false);

    proxy_server.stop();
    rs.stopSet();
};

export const testProxyProtocolReplicaSetWithProxyUnixSocket = (ingressPort, testFn) => {
    const prefix = `${MongoRunner.dataPath}${jsTestName()}`;
    mkdir(prefix);

    const rs = new ReplSetTest({nodes: 1});
    rs.startSet({proxyUnixSocketPrefix: prefix, unixSocketPrefix: prefix});
    rs.initiate();

    const unixSockPath = `${prefix}/proxy-mongodb-${rs.getPrimary().port}.sock`;
    const proxy_server = new ProxyProtocolServer(
        ingressPort,
        "" /* egressPort (ignored) */,
        2 /* proxy protocol version */,
        {egressUnixSocket: unixSockPath},
    );
    proxy_server.setTLVs([{"type": 0x02, "value": "authority.example.com"}]);
    proxy_server.start();

    testFn(ingressPort, prefix, rs.getPrimary(), false);

    proxy_server.stop();
    rs.stopSet();
};

export const testProxyProtocolShardedCluster = (ingressPort, egressPort, version, testFn) => {
    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxy_server.start();

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        mongosOptions: {setParameter: {"loadBalancerPort": egressPort}},
    });

    testFn(ingressPort, egressPort, st.s, true);

    proxy_server.stop();
    st.stop();
};

export const testProxyProtocolShardedClusterWithProxyUnixSocket = (ingressPort, testFn) => {
    const prefix = `${MongoRunner.dataPath}${jsTestName()}`;
    mkdir(prefix);

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        mongosOptions: {proxyUnixSocketPrefix: prefix, unixSocketPrefix: prefix},
    });

    const unixSockPath = `${prefix}/proxy-mongodb-${st.s0.port}.sock`;
    const proxy_server = new ProxyProtocolServer(
        ingressPort,
        "" /* egressPort (ignored) */,
        2 /* proxy protocol version */,
        {egressUnixSocket: unixSockPath},
    );
    proxy_server.setTLVs([{"type": 0x02, "value": "authority.example.com"}]);
    proxy_server.start();

    testFn(ingressPort, prefix, st.s, true);

    proxy_server.stop();
    st.stop();
};

// Verify that the "client metadata" log (id 51800) emits the remote attribute properly.
export const testClientMetadataLogOverUnixSocket = (ingressPort, unixSockPrefix, node, isRouter) => {
    const kClientMetadataLogId = 51800;
    const unixSockPath = `${unixSockPrefix}/mongodb-${node.port}.sock`;
    const proxyUnixSockPath = `${unixSockPrefix}/proxy-mongodb-${node.port}.sock`;

    // Connections via a unix domain socket should log "anonymous unix socket:27017" as remote attr.
    const directConn = new Mongo(unixSockPath);
    assert.neq(null, directConn, "Failed to connect directly to node");
    assert.commandWorked(directConn.getDB("admin").runCommand({hello: 1}));
    checkLog.containsJson(node, kClientMetadataLogId, {remote: "anonymous unix socket:27017"});

    // Connections via the proxy unix domain socket should log the originating address reported in the proxy protocol header.
    const lbParam = isRouter ? "/?loadBalanced=true" : "";
    const proxiedConn = new Mongo(`mongodb://127.0.0.1:${ingressPort}${lbParam}`);
    assert.neq(null, proxiedConn, "Failed to connect through proxy");
    assert.commandWorked(proxiedConn.getDB("admin").runCommand({hello: 1}));
    checkLog.containsJson(node, kClientMetadataLogId, {remote: /^127\.0\.0\.1:\d{1,5}$/});
};
