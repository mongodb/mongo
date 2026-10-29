/**
 * Verify mongod support proxy protocol connections.
 * @tags: [
 *    requires_fcv_83,
 *    grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}
import {
    emptyMessageTest,
    failX509Auth,
    fuzzingTest,
    caFile,
    keyfile,
    newTLSMongo,
    runAsAdminUser,
    serverCertFile,
    setupAuth,
    succeedX509Auth,
    testProxyProtocolReplicaSet,
    testProxyProtocolReplicaSetWithProxyUnixSocket,
    testClientMetadataLogOverUnixSocket,
} from "jstests/noPassthrough/libs/proxy_protocol_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

function sendHelloMaybeLB(node, port, loadBalanced, count) {
    const kLoadBalancerNoOpMessage = 10107800;
    const conn = newTLSMongo(
        `127.0.0.1:${port}`,
        typeof loadBalanced != "undefined" ? [`loadBalanced=${loadBalanced}`] : [],
    );
    assert.neq(null, conn, "Client was unable to connect to the load balancer port");
    assert.commandWorked(conn.getDB("admin").runCommand({hello: 1}));

    if (loadBalanced) {
        runAsAdminUser(node, (connection) => {
            assert(
                checkLog.checkContainsWithCountJson(
                    connection,
                    kLoadBalancerNoOpMessage,
                    {},
                    count,
                    undefined,
                    true,
                ),
                `Did not find log id ${kLoadBalancerNoOpMessage} ${tojson(count)} times in the log`,
            );
        });
    }
}

function failInvalidProtocol(node, port, id, attrs, loadBalanced, count) {
    const options =
        typeof loadBalanced != "undefined" ? [`loadBalanced=${tojson(loadBalanced)}`] : [];
    try {
        newTLSMongo(`127.0.0.1:${port}`, options);
        assert(false, "Client was unable to connect to the load balancer port");
    } catch (err) {
        let actualCount;
        const compareCounts = (actual, expected) => {
            // Capture the actual number of times a matching log entry was found in the mongod log.
            // This way we can mention it if the assertion fails below.
            actualCount = actual;
            return actual === expected;
        };
        runAsAdminUser(node, (connection) => {
            assert(
                checkLog.checkContainsWithCountJson(
                    connection,
                    id,
                    attrs,
                    count,
                    undefined,
                    true,
                    compareCounts,
                ),
                `Did not find log id ${tojson(id)} with attr ${tojson(attrs)} in the log the expected number of times. Expected to see it ${count} times but saw it ${actualCount} times. This assertion failed while handling an expected error. The error was: ${tojson(err)}`,
            );
        });
    }
}

// Test that you can connect to the load balancer port over a proxy.
function basicTest(ingressPort, egressPort, node) {
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
}

function standardPortTest(ingressPort, egressPort, version) {
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
            "logComponentVerbosity": {"network": {"verbosity": 5}},
            "proxyProtocolTimeoutSecs": 10,
            "proxyProtocolMaximumWaitBackoffMillis": 500,
        },
    });
    rs.initiate();

    const node = rs.getPrimary();
    setupAuth(node);
    const proxy_server = new ProxyProtocolServer(ingressPort, node.port, version, {
        ingressTLSCert: serverCertFile,
        ingressTLSCA: caFile,
    });
    proxy_server.start();
    const attrs = {
        "error": {
            "code": ErrorCodes.OperationFailed,
            "codeName": "OperationFailed",
            "errmsg": "ProxyProtocol message detected on mongorpc port",
        },
    };
    failInvalidProtocol(node, ingressPort, 22988, attrs, true, 1);
    failInvalidProtocol(node, ingressPort, 22988, attrs, false, 2);
    failInvalidProtocol(node, ingressPort, 22988, attrs, false, 3);
    proxy_server.stop();
    rs.stopSet();
}

const ingressPort = allocatePort();
const egressPort = allocatePort();

testProxyProtocolReplicaSet(ingressPort, egressPort, 1, basicTest);
testProxyProtocolReplicaSet(ingressPort, egressPort, 2, basicTest);

standardPortTest(ingressPort, egressPort, 1);
standardPortTest(ingressPort, egressPort, 2);

testProxyProtocolReplicaSet(ingressPort, egressPort, 1, emptyMessageTest);
testProxyProtocolReplicaSet(ingressPort, egressPort, 2, emptyMessageTest);

testProxyProtocolReplicaSet(ingressPort, egressPort, 1, fuzzingTest);
testProxyProtocolReplicaSet(ingressPort, egressPort, 2, fuzzingTest);

// Mongod should accept connections presenting Proxy Protocol v2
// headers with SSL TLVs via the proxy port but ignore parsing the TLVs.
// Subsequently, X.509 auth will fail.
testProxyProtocolReplicaSet(ingressPort, egressPort, 2, failX509Auth);

testProxyProtocolReplicaSetWithProxyUnixSocket(ingressPort, testClientMetadataLogOverUnixSocket);

// Mongod should successfully parse SSL TLVs via the proxy UDS, allowing
// for X.509 auth.
testProxyProtocolReplicaSetWithProxyUnixSocket(ingressPort, succeedX509Auth);
