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
    const conn = new Mongo(`mongodb://127.0.0.1:${port}${isRouter ? '/?loadBalanced=true' : ''}`);
    assert.neq(null, conn, `Client was unable to connect to port ${port}`);
    assert.lt(Date.now() - connStart, 10 * 1000, 'Client was unable to connect within 10 seconds');
    assert.commandWorked(conn.getDB('admin').runCommand({hello: 1}));
};

export const timeoutEmptyConnection = (ingressPort, egressPort, isRouter) => {
    // Use the connection to set a lower proxy header timeout and validate that empty connections
    // timeout.
    const conn =
        new Mongo(`mongodb://127.0.0.1:${ingressPort}${isRouter ? '/?loadBalanced=true' : ''}`);
    const proxyTimeoutFailPoint = configureFailPoint(conn, "asioTransportLayer1sProxyTimeout");

    // runProgram blocks until the program is complete. nc should be finished when the server times
    // out the connection that doesn't send data after 1 second, otherwise the test will hang.
    assert.eq(0, runProgram("bash", "-c", `cat </dev/tcp/127.0.0.1/${egressPort}`));

    proxyTimeoutFailPoint.off();
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
    const numConnections = 200;

    for (let i = 0; i < numConnections; i++) {
        jsTestLog("Sending random data to proxy port");
        const pid = _startMongoProgram(
            'bash',
            '-c',
            `head -c ${Math.floor(Math.random() * 5000)} /dev/urandom >/dev/tcp/127.0.0.1/${
                egressPort}`);

        // Connecting to the to the proxy port still succeeds within a reasonable time
        // limit.
        connectAndHello(ingressPort, isRouter);

        // Connecting to the default port still succeeds within a reasonable time limit.
        connectAndHello(node.port, isRouter);

        assert.soon(() => !checkProgram(pid).alive,
                    "Server should have closed connection with invalid proxy protocol header");
    }
};

export const loadTest = (ingressPort, egressPort, node, isRouter) => {
    const numConnections = 200;
    let threads = [];

    for (let i = 0; i < numConnections; i++) {
        threads.push(new Thread((regularPort, ingressPort, egressPort, connectFn, isRouter) => {
            // Throw in some connections without data to make sure we handle those correctly.
            const pid =
                _startMongoProgram("bash", "-c", `exec cat < /dev/tcp/127.0.0.1/${egressPort}`);

            // Connecting to the proxy port still succeeds within a reasonable time
            // limit.
            connectFn(ingressPort, isRouter);

            // Connecting to the default port still succeeds within a reasonable time limit.
            connectFn(regularPort, isRouter);

            assert(checkProgram(pid).alive);
            stopMongoProgramByPid(pid);
        }, node.port, ingressPort, egressPort, connectAndHello, isRouter));
        threads[i].start();
    }

    for (let i = 0; i < numConnections; i++) {
        threads[i].join();
    }
};

export const testProxyProtocolReplicaSet = (ingressPort, egressPort, version, testFn) => {
    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxy_server.start();

    const rs = new ReplSetTest({nodes: 1, nodeOptions: {"proxyPort": egressPort}});
    rs.startSet({setParameter: {featureFlagMongodProxyProtocolSupport: true}});
    rs.initiate();

    testFn(ingressPort, egressPort, rs.getPrimary(), false);

    proxy_server.stop();
    rs.stopSet();
};

export const testProxyProtocolShardedCluster = (ingressPort, egressPort, version, testFn) => {
    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxy_server.start();

    const st = new ShardingTest(
        {shards: 1, mongos: 1, mongosOptions: {setParameter: {"loadBalancerPort": egressPort}}});

    testFn(ingressPort, egressPort, st.s, true);

    proxy_server.stop();
    st.stop();
};
