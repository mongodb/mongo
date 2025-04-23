/**
 * Validate we can connect over the proxy protocol port with the protocol appended.
 * @tags: [
 *   requires_fcv_52,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertContainsOnceJsonStringMatch(connOrFile, id, attrName, attrText, errorMsg) {
    const quote = JSON.stringify;
    const foundMatch =
        checkLog.checkContainsOnceJsonStringMatch(connOrFile, id, attrName, attrText);
    const fullErrorMsg = `${errorMsg}: ${quote(attrText)} not found in the ${
        quote(attrName)} attribute of log messages having ID ${id}`;
    assert(foundMatch, fullErrorMsg);
}

// Test that you can connect to the load balancer port over a proxy.
function testProxyProtocolConnect(ingressPort, egressPort, version) {
    let proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxy_server.start();

    let st = new ShardingTest(
        {shards: 1, mongos: 1, mongosOptions: {setParameter: {"loadBalancerPort": egressPort}}});

    const uri = `mongodb://127.0.0.1:${ingressPort}/?loadBalanced=true`;
    const conn = new Mongo(uri);
    const mongoShellPort = conn.getShellPort();
    const proxyServerPort = proxy_server.getServerPort();

    assert.neq(null, conn, 'Client was unable to connect to the load balancer port');
    assert.commandWorked(conn.getDB('admin').runCommand({hello: 1}));

    const fcv = conn.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    if (fcv.featureCompatibilityVersion.version === latestFCV) {
        assertContainsOnceJsonStringMatch(
            st.s, 22943, "isLoadBalanced", "true", "isLoadBalanced was set to false");
        assertContainsOnceJsonStringMatch(st.s,
                                          22943,
                                          "remote",
                                          `127.0.0.1:${proxyServerPort}`,
                                          "Remote had a different address");
        assertContainsOnceJsonStringMatch(
            st.s,
            22943,
            "sourceClient",
            `127.0.0.1:${mongoShellPort}`,
            "Source client was not included, or had a different address");
    }

    proxy_server.stop();
    st.stop();
}

// Test that you can't connect to the load balancer port without being proxied.
function testProxyProtocolConnectFailure(lbPort, sendLoadBalanced) {
    let st = new ShardingTest(
        {shards: 1, mongos: 1, mongosOptions: {setParameter: {"loadBalancerPort": lbPort}}});

    const hostName = st.s.host.substring(0, st.s.host.indexOf(":"));
    const uri = `mongodb://${hostName}:${lbPort}/?loadBalanced=${sendLoadBalanced}`;
    try {
        new Mongo(uri);
        assert(false, 'Client was unable to connect to the load balancer port');
    } catch (err) {
        assert(checkLog.checkContainsOnceJsonStringMatch(
                   st.s, 6067900, "msg", "Error while parsing proxy protocol header"),
               "Connection failed for some reason other than lacking a proxy protocol header");
    }
    st.stop();
}

const ingressPort = allocatePort();
const egressPort = allocatePort();

testProxyProtocolConnect(ingressPort, egressPort, 1);
testProxyProtocolConnect(ingressPort, egressPort, 2);
testProxyProtocolConnectFailure(egressPort, "true");
testProxyProtocolConnectFailure(egressPort, "false");
