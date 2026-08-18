/**
 * Verify mongos supports proxy protocol connections.
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
    succeedX509Auth,
    testProxyProtocolShardedCluster,
    testProxyProtocolShardedClusterWithProxyUnixSocket,
    testClientMetadataLogOverUnixSocket,
} from "jstests/noPassthrough/libs/proxy_protocol_helpers.js";

const ingressPort = allocatePort();
const egressPort = allocatePort();

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, emptyMessageTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, emptyMessageTest);

// Mongos should accept connections presenting Proxy Protocol v2
// headers with SSL TLVs via the load balancer port but ignore parsing the TLVs.
// Subsequently, X.509 auth will fail.
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, failX509Auth);

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, fuzzingTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, fuzzingTest);

testProxyProtocolShardedClusterWithProxyUnixSocket(
    ingressPort,
    testClientMetadataLogOverUnixSocket,
);

// Mongos should successfully parse SSL TLVs via the proxy UDS, allowing
// for X.509 auth.
testProxyProtocolShardedClusterWithProxyUnixSocket(ingressPort, succeedX509Auth);
