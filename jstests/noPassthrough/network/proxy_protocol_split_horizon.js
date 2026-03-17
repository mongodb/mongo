/**
 * Tests that split horizon works correctly when the SNI is forwarded via the PROXY protocol v2
 * Authority TLV (0x02).
 *
 * Architecture:
 *   [mongo shell] --TCP--> [proxy protocol server] --PP2--> [mongod proxy port]
 *
 * The proxy injects the Authority TLV carrying the SNI hostname. mongod's proxy protocol parser
 * extracts the SNI and stores it in SSLPeerInfo. The hello command then uses that SNI to
 * resolve the split horizon, returning the appropriate horizon-specific hostnames in its response.
 *
 * @tags: [
 *   grpc_incompatible,
 *   multiversion_incompatible,
 *   requires_replication
 * ]
 */

if (_isWindows()) {
    quit();
}

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

// --- Constants ---
const kHorizonName = "proxy_horizon";
const kHorizonSNI = "proxy-horizon.example.com";
const kUnknownSNI = "unknown-host.example.com";

// --- Allocate ports for the proxy protocol servers (one per replica set member). ---
const kProxyIngressPort0 = allocatePort();
const kProxyIngressPort1 = allocatePort();
const kProxyVersion = 2;
const kSocketPrefix = `${MongoRunner.dataDir}/proxy_split_horizon_sockets`;
mkdir(kSocketPrefix);

// --- Start a 2-node replica set with proxy ports enabled. ---
const replTest = new ReplSetTest({
    name: "proxyHorizonTest",
    nodes: 2,
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojson({network: {verbosity: 2}}),
        },
    },
    host: "localhost",
    useHostName: false,
});

// Each node gets its own proxyUnixSocketPrefix.
replTest.startSet({}, false, /* overrideByNodeOptions */ true);

// We need to restart nodes with proxyUnixSocketPrefix set. ReplSetTest doesn't support per-node
// proxyUnixSocketPrefix in nodeOptions directly, so we restart with the correct config.

// Stop and restart with proxy Unix socket prefix.
const node0Port = replTest.nodes[0].port;
const node1Port = replTest.nodes[1].port;

replTest.stop(0);
replTest.stop(1);

replTest.start(0, {proxyUnixSocketPrefix: kSocketPrefix});
replTest.start(1, {proxyUnixSocketPrefix: kSocketPrefix});

replTest.initiate();

// Configure horizons. The horizon hostnames use the actual proxy ingress ports so the test
// can verify the `me` and `hosts` fields match, even though we won't actually connect through
// them for DNS resolution (the proxy handles the PP2 forwarding).
const primary = replTest.getPrimary();
let config = replTest.getReplSetConfigFromNode();

const node0Host = `localhost:${node0Port}`;
const node1Host = `localhost:${node1Port}`;
const node0HorizonHost = `${kHorizonSNI}:${node0Port}`;
const node1HorizonHost = `${kHorizonSNI}:${node1Port}`;

config.version += 1;
config.members[0].horizons = {};
config.members[0].horizons[kHorizonName] = node0HorizonHost;
config.members[1].horizons = {};
config.members[1].horizons[kHorizonName] = node1HorizonHost;

assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
replTest.awaitReplication();

// --- Start proxy protocol servers for each node (egress via Unix domain sockets). ---
const uds0 = `${kSocketPrefix}/proxy-mongodb-${node0Port}.sock`;
assert(fileExists(uds0), `Proxy UDS should exist at ${uds0}`);
const proxy0 = new ProxyProtocolServer(kProxyIngressPort0, node0Port, kProxyVersion, {
    egressUnixSocket: uds0,
});
proxy0.start();

const uds1 = `${kSocketPrefix}/proxy-mongodb-${node1Port}.sock`;
assert(fileExists(uds1), `Proxy UDS should exist at ${uds1}`);
const proxy1 = new ProxyProtocolServer(kProxyIngressPort1, node1Port, kProxyVersion, {
    egressUnixSocket: uds1,
});
proxy1.start();

/**
 * Connects through the proxy to the given ingress port and runs hello, returning the response.
 *
 * @param {ProxyProtocolServer} proxy - The proxy server to set TLVs on.
 * @param {number} ingressPort - Port to connect through.
 * @param {string|null} sni - SNI to inject via Authority TLV, or null for no SNI.
 * @returns {Object} The hello command response.
 */
function helloViaProxy(proxy, ingressPort, sni) {
    const tlvs = [];
    if (sni) {
        tlvs.push({"type": 0x02, "value": sni});
    }
    // Always include at least one TLV so the PP2 header is well-formed.
    if (tlvs.length === 0) {
        tlvs.push({"type": 0x01, "value": "h2"}); // ALPN as filler
    }
    proxy.setTLVs(tlvs);

    const uri = `mongodb://127.0.0.1:${ingressPort}`;
    const conn = new Mongo(uri);
    const result = assert.commandWorked(conn.getDB("admin").runCommand({hello: 1}));
    return result;
}

// =============================================================================
// Test 1: No SNI → default horizon (localhost hostnames).
// =============================================================================
jsTest.log.info("Test 1: No SNI through proxy → default horizon");
{
    const result = helloViaProxy(proxy0, kProxyIngressPort0, null);
    jsTest.log.info("hello response (no SNI): " + tojson(result));

    // With no SNI, the server should return the default horizon hostnames.
    assert(result.hosts.includes(node0Host), `Expected hosts to contain ${node0Host}, got: ${tojson(result.hosts)}`);
    assert(result.hosts.includes(node1Host), `Expected hosts to contain ${node1Host}, got: ${tojson(result.hosts)}`);

    // 'me' should be the default (localhost) hostname for node 0.
    assert.eq(result.me, node0Host, `Expected 'me' to be ${node0Host}, got: ${result.me}`);
}

// =============================================================================
// Test 2: SNI matching the horizon → horizon hostnames returned.
// =============================================================================
jsTest.log.info("Test 2: SNI matching horizon through proxy → horizon hostnames");
{
    const result = helloViaProxy(proxy0, kProxyIngressPort0, kHorizonSNI);
    jsTest.log.info("hello response (horizon SNI): " + tojson(result));

    // With the horizon SNI, the server should return the horizon hostnames.
    assert(
        result.hosts.includes(node0HorizonHost),
        `Expected hosts to contain ${node0HorizonHost}, got: ${tojson(result.hosts)}`,
    );
    assert(
        result.hosts.includes(node1HorizonHost),
        `Expected hosts to contain ${node1HorizonHost}, got: ${tojson(result.hosts)}`,
    );

    // 'me' should be the horizon hostname for node 0.
    assert.eq(result.me, node0HorizonHost, `Expected 'me' to be ${node0HorizonHost}, got: ${result.me}`);
}

// =============================================================================
// Test 3: SNI matching the horizon on the second node → same horizon, different 'me'.
// =============================================================================
jsTest.log.info("Test 3: SNI matching horizon on node 1 → horizon hostnames, me=node1");
{
    const result = helloViaProxy(proxy1, kProxyIngressPort1, kHorizonSNI);
    jsTest.log.info("hello response (horizon SNI, node 1): " + tojson(result));

    assert(
        result.hosts.includes(node0HorizonHost),
        `Expected hosts to contain ${node0HorizonHost}, got: ${tojson(result.hosts)}`,
    );
    assert(
        result.hosts.includes(node1HorizonHost),
        `Expected hosts to contain ${node1HorizonHost}, got: ${tojson(result.hosts)}`,
    );

    // 'me' should be the horizon hostname for node 1.
    assert.eq(result.me, node1HorizonHost, `Expected 'me' to be ${node1HorizonHost}, got: ${result.me}`);
}

// =============================================================================
// Test 4: Unknown SNI → falls back to default horizon.
// =============================================================================
jsTest.log.info("Test 4: Unknown SNI through proxy → default horizon (fallback)");
{
    const result = helloViaProxy(proxy0, kProxyIngressPort0, kUnknownSNI);
    jsTest.log.info("hello response (unknown SNI): " + tojson(result));

    // An SNI that doesn't match any horizon should fall back to the default.
    assert(result.hosts.includes(node0Host), `Expected hosts to contain ${node0Host}, got: ${tojson(result.hosts)}`);
    assert(result.hosts.includes(node1Host), `Expected hosts to contain ${node1Host}, got: ${tojson(result.hosts)}`);

    assert.eq(result.me, node0Host, `Expected 'me' to be ${node0Host}, got: ${result.me}`);
}

// =============================================================================
// Test 5: Reconfigure horizons and verify proxy-forwarded SNI picks up the change.
// =============================================================================
jsTest.log.info("Test 5: Reconfigure horizons, verify proxy SNI uses new config");
{
    // Add a second horizon using a different SNI.
    const kSecondHorizonName = "second_horizon";
    const kSecondHorizonSNI = "second-horizon.example.com";
    const node0SecondHorizonHost = `${kSecondHorizonSNI}:${node0Port}`;
    const node1SecondHorizonHost = `${kSecondHorizonSNI}:${node1Port}`;

    let newConfig = replTest.getReplSetConfigFromNode();
    newConfig.version += 1;
    newConfig.members[0].horizons[kSecondHorizonName] = node0SecondHorizonHost;
    newConfig.members[1].horizons[kSecondHorizonName] = node1SecondHorizonHost;

    assert.commandWorked(replTest.getPrimary().adminCommand({replSetReconfig: newConfig}));
    replTest.awaitReplication();

    // The original horizon SNI should still work.
    const result1 = helloViaProxy(proxy0, kProxyIngressPort0, kHorizonSNI);
    assert(
        result1.hosts.includes(node0HorizonHost),
        `After reconfig: expected hosts to contain ${node0HorizonHost}, got: ${tojson(result1.hosts)}`,
    );
    assert.eq(
        result1.me,
        node0HorizonHost,
        `After reconfig: expected 'me' to be ${node0HorizonHost}, got: ${result1.me}`,
    );

    // The new horizon SNI should now work.
    const result2 = helloViaProxy(proxy0, kProxyIngressPort0, kSecondHorizonSNI);
    assert(
        result2.hosts.includes(node0SecondHorizonHost),
        `After reconfig: expected hosts to contain ${node0SecondHorizonHost}, got: ${tojson(result2.hosts)}`,
    );
    assert(
        result2.hosts.includes(node1SecondHorizonHost),
        `After reconfig: expected hosts to contain ${node1SecondHorizonHost}, got: ${tojson(result2.hosts)}`,
    );
    assert.eq(
        result2.me,
        node0SecondHorizonHost,
        `After reconfig: expected 'me' to be ${node0SecondHorizonHost}, got: ${result2.me}`,
    );

    // The previously unknown SNI should still fall back to default.
    const result3 = helloViaProxy(proxy0, kProxyIngressPort0, kUnknownSNI);
    assert(
        result3.hosts.includes(node0Host),
        `After reconfig: expected hosts to contain ${node0Host}, got: ${tojson(result3.hosts)}`,
    );
    assert.eq(result3.me, node0Host, `After reconfig: expected 'me' to be ${node0Host}, got: ${result3.me}`);
}

// --- Cleanup ---
proxy0.stop();
proxy1.stop();

replTest.stopSet();
