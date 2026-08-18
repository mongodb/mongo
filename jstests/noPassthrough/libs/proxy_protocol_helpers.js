/**
 * Helpers for testing the proxy protocol.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

export const keyfile = "jstests/libs/key1";
export const serverCertFile = "jstests/libs/server.pem";
export const caFile = "jstests/libs/ca.pem";
export const clientCertFile = "jstests/libs/client.pem";
export const x509_user = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

export const tlsClientOptions = {
    tls: {
        certificateKeyFile: clientCertFile,
        CAFile: caFile,
        allowInvalidHostnames: true,
    },
};

export function newTLSMongo(hostAndPort, options = []) {
    const uri = `mongodb://${hostAndPort}/?${["tls=true", ...options].join("&")}`;
    return new Mongo(uri, undefined, tlsClientOptions);
}

export function setupAuth(conn) {
    // Create a root user for admin ops and an X.509 user to test X.509
    // auth via proxy protocol TLVs.
    assert.commandWorked(conn.adminCommand({createUser: "admin", pwd: "pwd", roles: ["root"]}));
    runAsAdminUser(conn, (connection) => {
        assert.commandWorked(
            connection.getDB("$external").runCommand({createUser: x509_user, roles: []}),
        );
    });
}

export function runAsAdminUser(conn, fn, uriOptions = []) {
    let newConn = newTLSMongo(conn.host, uriOptions);
    assert(newConn.getDB("admin").auth("admin", "pwd"), "Authentication failed");
    const res = fn(newConn);
    newConn.close();
    return res;
}

export const connectAndHello = (port, isRouter) => {
    jsTestLog(`Attempting to connect to port ${port}`);
    const connStart = Date.now();
    const conn = newTLSMongo(`127.0.0.1:${port}`, isRouter ? ["loadBalanced=true"] : []);
    assert.neq(null, conn, `Client was unable to connect to port ${port}`);
    assert.lt(Date.now() - connStart, 10 * 1000, "Client was unable to connect within 10 seconds");
    assert.commandWorked(conn.getDB("admin").runCommand({hello: 1}));
};

export const succeedX509Auth = (ingressPort, unixSockPrefix, node, isRouter) => {
    const conn = newTLSMongo(`127.0.0.1:${ingressPort}`, isRouter ? ["loadBalanced=true"] : []);
    const externalDB = conn.getDB("$external");
    assert(externalDB.auth({user: x509_user, mechanism: "MONGODB-X509"}));
    conn.close();
};

export const failX509Auth = (ingressPort, egressPort, node, isRouter) => {
    const conn = newTLSMongo(`127.0.0.1:${ingressPort}`, isRouter ? ["loadBalanced=true"] : []);
    const externalDB = conn.getDB("$external");
    assert(!externalDB.auth({user: x509_user, mechanism: "MONGODB-X509"}));
    conn.close();
};

export const timeoutEmptyConnection = (ingressPort, egressPort, isRouter) => {
    // Use the connection to set a lower proxy header timeout and validate that empty connections
    // timeout.
    const conn = newTLSMongo(`127.0.0.1:${ingressPort}`, isRouter ? ["loadBalanced=true"] : []);
    const previousParameter = runAsAdminUser(conn, (connection) => {
        return connection.adminCommand({getParameter: 1, proxyProtocolTimeoutSecs: 1});
    });
    runAsAdminUser(conn, (connection) => {
        connection.adminCommand({setParameter: 1, proxyProtocolTimeoutSecs: 1});
    });

    // runProgram blocks until the program is complete. nc should be finished when the server times
    // out the connection that doesn't send data after 1 second, otherwise the test will hang.
    assert.eq(0, runProgram("bash", "-c", `cat </dev/tcp/127.0.0.1/${egressPort}`));

    runAsAdminUser(conn, (connection) => {
        connection.adminCommand({
            setParameter: 1,
            proxyProtocolTimeoutSecs: previousParameter.proxyProtocolTimeoutSecs,
        });
    });
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
    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version, {
        ingressTLSCert: serverCertFile,
        ingressTLSCA: caFile,
    });
    proxy_server.setTLVs([
        {"type": 0x02, "value": "authority.example.com"},
        {
            "ssl": [{"type": 0xe0, "value": x509_user}],
        },
    ]);
    proxy_server.start();

    const rs = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            "proxyPort": egressPort,
            tlsCertificateKeyFile: serverCertFile,
            tlsCAFile: caFile,
            tlsMode: "allowTLS",
            tlsAllowInvalidHostnames: "",
        },
        keyFile: keyfile,
    });
    rs.startSet({
        setParameter: {
            featureFlagMongodProxyProtocolSupport: true,
            "logComponentVerbosity": {network: 5},
        },
    });
    rs.initiate();

    const primary = rs.getPrimary();
    setupAuth(primary);
    testFn(ingressPort, egressPort, primary, false);

    proxy_server.stop();
    rs.stopSet();
};

export const testProxyProtocolReplicaSetWithProxyUnixSocket = (ingressPort, testFn) => {
    const prefix = `${MongoRunner.dataPath}${jsTestName()}`;
    mkdir(prefix);

    const rs = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            tlsCertificateKeyFile: serverCertFile,
            tlsCAFile: caFile,
            tlsMode: "allowTLS",
            tlsAllowInvalidHostnames: "",
        },
    });
    rs.startSet({proxyUnixSocketPrefix: prefix, unixSocketPrefix: prefix});
    rs.initiate();

    const unixSockPath = `${prefix}/proxy-mongodb-${rs.getPrimary().port}.sock`;
    const proxy_server = new ProxyProtocolServer(
        ingressPort,
        "" /* egressPort (ignored) */,
        2 /* proxy protocol version */,
        {
            ingressTLSCert: serverCertFile,
            ingressTLSCA: caFile,
            egressUnixSocket: unixSockPath,
        },
    );
    proxy_server.setTLVs([
        {"type": 0x02, "value": "authority.example.com"},
        {
            "ssl": [{"type": 0xe0, "value": x509_user}],
        },
    ]);
    proxy_server.start();

    const primary = rs.getPrimary();
    setupAuth(primary);
    testFn(ingressPort, prefix, primary, false);

    proxy_server.stop();
    rs.stopSet();
};

export const testProxyProtocolShardedCluster = (ingressPort, egressPort, version, testFn) => {
    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version, {
        ingressTLSCert: serverCertFile,
        ingressTLSCA: caFile,
    });
    proxy_server.setTLVs([
        {"type": 0x02, "value": "authority.example.com"},
        {
            "ssl": [{"type": 0xe0, "value": x509_user}],
        },
    ]);
    proxy_server.start();

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        mongosOptions: {
            setParameter: {"loadBalancerPort": egressPort},
            tlsCertificateKeyFile: serverCertFile,
            tlsCAFile: caFile,
            tlsMode: "allowTLS",
            tlsAllowInvalidHostnames: "",
        },
        keyFile: keyfile,
    });
    setupAuth(st.s);

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
        mongosOptions: {
            proxyUnixSocketPrefix: prefix,
            unixSocketPrefix: prefix,
            tlsCertificateKeyFile: serverCertFile,
            tlsCAFile: caFile,
            tlsMode: "allowTLS",
            tlsAllowInvalidHostnames: "",
        },
        keyFile: keyfile,
    });
    setupAuth(st.s);

    const unixSockPath = `${prefix}/proxy-mongodb-${st.s0.port}.sock`;
    const proxy_server = new ProxyProtocolServer(
        ingressPort,
        "" /* egressPort (ignored) */,
        2 /* proxy protocol version */,
        {
            ingressTLSCert: serverCertFile,
            ingressTLSCA: caFile,
            egressUnixSocket: unixSockPath,
        },
    );
    proxy_server.setTLVs([
        {"type": 0x02, "value": "authority.example.com"},
        {
            "ssl": [{"type": 0xe0, "value": x509_user}],
        },
    ]);
    proxy_server.start();

    testFn(ingressPort, prefix, st.s, true);

    proxy_server.stop();
    st.stop();
};

// Verify that the "client metadata" log (id 51800) emits the remote attribute properly.
export const testClientMetadataLogOverUnixSocket = (
    ingressPort,
    unixSockPrefix,
    node,
    isRouter,
) => {
    const kClientMetadataLogId = 51800;
    const unixSockPath = `${unixSockPrefix}/mongodb-${node.port}.sock`;
    const proxyUnixSockPath = `${unixSockPrefix}/proxy-mongodb-${node.port}.sock`;

    // Connections via a unix domain socket should log "anonymous unix socket" as remote attr.
    const directConn = new Mongo(unixSockPath);
    assert.neq(null, directConn, "Failed to connect directly to node");
    assert.commandWorked(directConn.getDB("admin").runCommand({hello: 1}));
    runAsAdminUser(node, (connection) => {
        checkLog.containsJson(connection, kClientMetadataLogId, {remote: "anonymous unix socket"});
    });

    // Connections via the proxy unix domain socket should log the originating address reported in the proxy protocol header.
    const proxiedConn = newTLSMongo(
        `127.0.0.1:${ingressPort}`,
        isRouter ? ["loadBalanced=true"] : [],
    );
    assert.neq(null, proxiedConn, "Failed to connect through proxy");
    assert.commandWorked(proxiedConn.getDB("admin").runCommand({hello: 1}));
    runAsAdminUser(node, (connection) => {
        checkLog.containsJson(connection, kClientMetadataLogId, {remote: /^127\.0\.0\.1:\d{1,5}$/});
    });
};
