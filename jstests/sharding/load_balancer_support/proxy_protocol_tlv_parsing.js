/**
 * Tests that a proxy protocol v2 connection with TLVs is correctly parsed and logged
 * when connecting through a load balancer port.
 * @tags: [
 *   multiversion_incompatible
 * ]
 */

if (_isWindows()) {
    quit();
}

import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

const kProxyIngressPort = allocatePort();
const kProxyEgressPort = allocatePort();
const kProxyVersion = 2;

let proxyServer = new ProxyProtocolServer(kProxyIngressPort, kProxyEgressPort, kProxyVersion);
proxyServer.start();

proxyServer.setTLVs([
    {"type": "0x02", "value": "authority.example.com"},
    {"type": "0xE0", "value": "custom_tlv_data"},
    {"type": "0xE1", "value": "hello_tlv_data"},
]);

let st = new ShardingTest({
    shards: 1,
    mongos: 1,
    mongosOptions: {
        setParameter: {
            "loadBalancerPort": kProxyEgressPort,
        },
    },
});

const admin = st.s.getDB("admin");

// Enable the isConnectedToProxyUnixSocketOverride failpoint so that the proxy protocol header
// parsing logic treats this TCP connection as if it arrived on a proxy unix socket. TLV parsing
// happens only for such connections.
assert.commandWorked(
    admin.adminCommand({configureFailPoint: "isConnectedToProxyUnixSocketOverride", mode: "alwaysOn"}),
);

// Increase network log verbosity so we can see log 11978400 emitted.
assert.commandWorked(
    admin.runCommand({
        setParameter: 1,
        logComponentVerbosity: {network: {verbosity: 4}},
    }),
);

// Connecting to the proxy will parse the header and log.
const uri = `mongodb://127.0.0.1:${kProxyIngressPort}/?loadBalanced=true`;
const proxiedConn = new Mongo(uri);

// Verify that log line 11978400 is emitted at least once with the expected data after at most 30 seconds.
checkLog.containsRelaxedJson(
    st.s,
    11978400,
    {
        "tlvs": "0x02:authority\.example\.com,0xe0:custom_tlv_data,0xe1:hello_tlv_data",
    },
    1,
    30 * 1000,
);

assert.commandWorked(admin.adminCommand({configureFailPoint: "isConnectedToProxyUnixSocketOverride", mode: "off"}));
assert.commandWorked(
    admin.runCommand({
        setParameter: 1,
        logComponentVerbosity: {network: {verbosity: 0}},
    }),
);

st.stop();
proxyServer.stop();
