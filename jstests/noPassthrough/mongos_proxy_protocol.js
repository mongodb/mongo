/**
 * Verify mongos supports proxy protocol connections.
 * @tags: [
 *    requires_fcv_80,
 * ]
 */

import {
    emptyMessageTest,
    fuzzingTest,
    testProxyProtocolShardedCluster,
    testProxyProtocolShardedClusterWithProxyUnixSocket,
    testClientMetadataLogOverUnixSocket,
} from "jstests/noPassthrough/libs/proxy_protocol_helpers.js";

if (_isWindows()) {
    quit();
}

const ingressPort = allocatePort();
const egressPort = allocatePort();

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, emptyMessageTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, emptyMessageTest);

testProxyProtocolShardedCluster(ingressPort, egressPort, 1, fuzzingTest);
testProxyProtocolShardedCluster(ingressPort, egressPort, 2, fuzzingTest);

testProxyProtocolShardedClusterWithProxyUnixSocket(ingressPort, testClientMetadataLogOverUnixSocket);
