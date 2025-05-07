/**
 *  Verify mongod support proxy protocol connections.
 * @tags: [
 *   requires_fcv_81,
 *    grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function sendHelloMaybeLB(node, port, loadBalanced, count) {
    const kLoadBalancerNoOpMessage = 10107800;
    let uri = `mongodb://127.0.0.1:${port}`;
    if (typeof loadBalanced != 'undefined') {
        uri += `/?loadBalanced=${loadBalanced}`;
    }
    const conn = new Mongo(uri);
    assert.neq(null, conn, 'Client was unable to connect to the load balancer port');
    assert.commandWorked(conn.getDB('admin').runCommand({hello: 1}));

    if (loadBalanced) {
        assert(checkLog.checkContainsWithCountJson(node, 10107800, {}, count, undefined, true),
               `Did not find log id 10107800 ${tojson(count)} times in the log`);
    }
}

function failInvalidProtocol(node, port, id, attrs, loadBalanced, count) {
    let uri = `mongodb://127.0.0.1:${port}`;
    if (typeof loadBalanced != 'undefined') {
        uri += `/?loadBalanced=${tojson(loadBalanced)}`;
    }
    try {
        new Mongo(uri);
        assert(false, 'Client was unable to connect to the load balancer port');
    } catch (err) {
        assert(checkLog.checkContainsWithCountJson(node, id, attrs, count, undefined, true),
               `Did not find log id ${tojson(id)} with attr ${tojson(attrs)} ${
                   tojson(id)} times in the log`);
    }
}

// Test that you can connect to the load balancer port over a proxy.
function testProxyProtocolReplicaSet(ingressPort, egressPort, version) {
    let proxy_server = new ProxyProtocolServer(ingressPort, egressPort, version);
    proxy_server.start();

    let rs = new ReplSetTest({nodes: 1, nodeOptions: {"proxyPort": egressPort}});
    rs.startSet({setParameter: {featureFlagMongodProxyProtocolSupport: true}});
    rs.initiate();

    const node = rs.getPrimary();
    // Connecting to the to the proxy port succeeds.
    sendHelloMaybeLB(node, ingressPort, undefined, 0);
    sendHelloMaybeLB(node, ingressPort, false, 0);

    // Connecting to the to the proxy port with {loadBalanced: true} fails.
    sendHelloMaybeLB(node, ingressPort, true, 1);

    // Connecting to the standard port without proxy header succeeds.
    const port = node.port;
    sendHelloMaybeLB(node, port, undefined, 0);
    sendHelloMaybeLB(node, port, false, 0);

    // Connecting to the standard port without and with {loadBalanced:true} proxy header fails.
    sendHelloMaybeLB(node, port, true, 2);

    // Connecting to the proxy port without proxy header fails.
    const kProxyProtocolParseError = 6067900;
    failInvalidProtocol(node, egressPort, kProxyProtocolParseError, undefined, true, 1);
    failInvalidProtocol(node, egressPort, kProxyProtocolParseError, undefined, false, 2);
    failInvalidProtocol(node, egressPort, kProxyProtocolParseError, undefined, undefined, 3);

    proxy_server.stop();

    // Connecting to the standard port with proxy header fails.
    proxy_server = new ProxyProtocolServer(ingressPort, port, version);
    proxy_server.start();
    const attrs = {
        "error": {
            "code": ErrorCodes.OperationFailed,
            "codeName": "OperationFailed",
            "errmsg": "ProxyProtocol message detected on mongorpc port",
        }
    };
    failInvalidProtocol(node, ingressPort, 22988, attrs, true, 1);
    failInvalidProtocol(node, ingressPort, 22988, attrs, false, 2);
    failInvalidProtocol(node, ingressPort, 22988, attrs, false, 3);
    proxy_server.stop();

    rs.stopSet();
}

const ingressPort = allocatePort();
const egressPort = allocatePort();

testProxyProtocolReplicaSet(ingressPort, egressPort, 1);
testProxyProtocolReplicaSet(ingressPort, egressPort, 2);
