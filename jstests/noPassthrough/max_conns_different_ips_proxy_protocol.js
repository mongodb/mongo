/**
 * Tests that the max connections overrides are respected for exempt IPs if the sourceClient IP is
 * different from the load balancer IP.
 *
 * Maximum connection overrides are not implemented for gRPC.
 * @tags: [
 *      grpc_incompatible,
 * ]
 */
if (_isWindows()) {
    quit();
}

load("jstests/libs/host_ipaddr.js");
load("jstests/sharding/libs/proxy_protocol.js");

const ingressPort = allocatePort();
const egressPort = allocatePort();
const ip = get_ipaddr();
const kConfiguredMaxConns = 5;

// First run a test that exempts 127.0.0.1 from maxConns, and uses a public IP for the proxy server
// and 127.0.0.1 for the sourceClient address. Ensure that the connection is exempted.
{
    let rs = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            "proxyPort": egressPort,
            config: "jstests/noPassthrough/libs/max_conns_override_config.yaml"
        }
    });
    rs.startSet();
    rs.initiate();

    let proxy_server = new ProxyProtocolServer(ingressPort, egressPort, 1);
    proxy_server.egress_address = ip;
    proxy_server.start();

    let conns = [];

    // Go up to maxConns
    for (let i = 0; i < kConfiguredMaxConns - 1; i++) {
        const conn = new Mongo(`mongodb://127.0.0.1:${ingressPort}`);
        assert.neq(null, conn, 'Client was unable to connect');
        assert.commandWorked(conn.getDB('admin').runCommand({hello: 1}));
        conns.push(conn);
    }

    // Make sure the connection past maxConns succeeds.
    const conn = new Mongo(`mongodb://127.0.0.1:${ingressPort}`);
    assert.neq(null, conn, 'Client was unable to connect');
    assert.commandWorked(conn.getDB('admin').runCommand({hello: 1}));

    proxy_server.stop();

    rs.stopSet();
}

// Next run a test that exempts 127.0.0.1 from maxConns, and uses a 127.0.0.1 for the proxy server
// and a public IP for the sourceClient address. Ensure that the connection is NOT exempted.
{
    let rs = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            "proxyPort": egressPort,
            config: "jstests/noPassthrough/libs/max_conns_override_config.yaml"
        }
    });
    rs.startSet();
    rs.initiate();

    let proxy_server = new ProxyProtocolServer(ingressPort, egressPort, 1);
    proxy_server.ingress_address = ip;
    proxy_server.start();

    let conns = [];

    // Go up to maxConns
    for (let i = 0; i < kConfiguredMaxConns - 1; i++) {
        const conn = new Mongo(`mongodb://${ip}:${ingressPort}`);
        assert.neq(null, conn, 'Client was unable to connect');
        assert.commandWorked(conn.getDB('admin').runCommand({hello: 1}));
        conns.push(conn);
    }

    // Make sure the connection past maxConns fails.
    let conn;
    try {
        conn = new Mongo(`mongodb://${ip}:${ingressPort}`);
    } catch (e) {
    }
    assert.eq(null, conn, 'Client connected when it should have failed');

    proxy_server.stop();

    rs.stopSet();
}
