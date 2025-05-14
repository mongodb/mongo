/**
 * Verify mongos supports proxy protocol connections.
 * @tags: [
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 *    grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}
import {
    emptyMessageTest,
    fuzzingTest,
    loadTest,
    testProxyProtocolShardedCluster
} from "jstests/noPassthrough/libs/proxy_protocol_helpers.js";

const ingressPort = allocatePort();
const egressPort = allocatePort();

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, emptyMessageTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, emptyMessageTest);

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, fuzzingTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, fuzzingTest);

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, loadTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, loadTest);
