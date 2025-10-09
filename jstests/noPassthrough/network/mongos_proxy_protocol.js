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
    fuzzingTest,
    testProxyProtocolShardedCluster,
} from "jstests/noPassthrough/libs/proxy_protocol_helpers.js";

const ingressPort = allocatePort();
const egressPort = allocatePort();

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, emptyMessageTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, emptyMessageTest);

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, fuzzingTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, fuzzingTest);
