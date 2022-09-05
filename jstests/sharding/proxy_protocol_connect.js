/**
 * Validate we can connect over the proxy protocol port with the protocol appended.
 * @tags: [requires_fcv_52]
 */

(function() {
if (_isWindows()) {
    // The proxy protocol python package currently doesn't support Windows.
    return;
}
load("jstests/sharding/libs/proxy_protocol.js");

// Test that you can connect to the load balancer port over a proxy.
function testProxyProtocolConnect(ingressPort, egressPort, version) {
    'use strict';

    let proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxy_server.start();

    let st = new ShardingTest(
        {shards: 1, mongos: 1, mongosOptions: {setParameter: {"loadBalancerPort": egressPort}}});

    const uri = `mongodb://127.0.0.1:${ingressPort}/?loadBalanced=true`;
    const conn = new Mongo(uri);
    assert.neq(null, conn, 'Client was unable to connect to the load balancer port');
    assert.commandWorked(conn.getDB('admin').runCommand({hello: 1}));
    proxy_server.stop();
    st.stop();
}

// Test that you can't connect to the load balancer port without being proxied.
function testProxyProtocolConnectFailure(lbPort, sendLoadBalanced) {
    'use strict';

    let st = new ShardingTest(
        {shards: 1, mongos: 1, mongosOptions: {setParameter: {"loadBalancerPort": lbPort}}});

    const hostName = st.s.host.substring(0, st.s.host.indexOf(":"));
    const uri = `mongodb://${hostName}:${lbPort}/?loadBalanced=${sendLoadBalanced}`;
    try {
        var conn = new Mongo(uri);
        assert(false, 'Client was unable to connect to the load balancer port');
    } catch (err) {
        assert(checkLog.checkContainsOnceJsonStringMatch(
                   st.s, 6067900, "msg", "Error while parsing proxy protocol header"),
               "Connection failed for some reason other than lacking a proxy protocol header");
    }
    st.stop();
}

const ingressPort = 21234;
const egressPort = 21235;

testProxyProtocolConnect(ingressPort, egressPort, 1);
testProxyProtocolConnect(ingressPort, egressPort, 2);
testProxyProtocolConnectFailure(egressPort, "true");
testProxyProtocolConnectFailure(egressPort, "false");
})();
