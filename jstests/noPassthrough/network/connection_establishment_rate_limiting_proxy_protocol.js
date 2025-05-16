/**
 * Tests that the connection establishment rate-limiter exemptions are respected for exempt IPs if
 * the proxy protocol is in use and the sourceClient IP is different from the load balancer IP.
 *
 * IP-based overrides are not implemented for gRPC.
 * @tags: [
 *      grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}

import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

// TODO SERVER-104811: Update this test with assertions on metrics as well.

const ingressPort = allocatePort();
const egressPort = allocatePort();
const nonExemptIP = get_ipaddr();
const exemptIP = "127.0.0.1";

let rs = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        "proxyPort": egressPort,
        config: "jstests/noPassthrough/libs/max_conns_override_config.yaml",
        setParameter: {
            ingressConnectionEstablishmentRatePerSec: 1,
            ingressConnectionEstablishmentBurstSize: 1,
            ingressConnectionEstablishmentMaxQueueDepth: 0,
            maxEstablishingConnectionsOverride: {ranges: [exemptIP]},
            featureFlagRateLimitIngressConnectionEstablishment: true
        }
    }
});
rs.startSet();
rs.initiate();

// Start up a proxy protocol server with a non-exempt IP as its egress address. Ensure that the
// sourceClient (an exempted address in this case) is consulted for determining whether or not rate
// limiting should be applied.
{
    let proxy_server = new ProxyProtocolServer(ingressPort, egressPort, 1);
    proxy_server.egress_address = nonExemptIP;
    proxy_server.start();

    // Make sure multiple connections can get through past the rate limit.
    for (let i = 0; i < 10; i++) {
        const conn = new Mongo(`mongodb://${exemptIP}:${ingressPort}`);
        assert.neq(null, conn, 'Client was unable to connect');
    }

    proxy_server.stop();
}

// Let connections through again.
rs.getPrimary().adminCommand({
    setParameter: 1,
    ingressConnectionEstablishmentRatePerSec: 10,
    ingressConnectionEstablishmentBurstSize: 500,
});

// Start up a proxy protocol server with an exempt IP as its egress address. Ensure that non-exempt
// sourceClient IPs are still subject to rate limiting.
{
    let proxy_server = new ProxyProtocolServer(ingressPort, egressPort, 1);
    proxy_server.ingress_address = nonExemptIP;
    proxy_server.start();

    // Reset the rate limiter to use lower values again.
    rs.getPrimary().adminCommand({
        setParameter: 1,
        ingressConnectionEstablishmentRatePerSec: 1,
        ingressConnectionEstablishmentBurstSize: 1,
    });

    // One token will be consumed by a non-exempt IP.
    assert(new Mongo(`mongodb://${nonExemptIP}:${ingressPort}`));

    // The connection attempts will outpace the refreshRate of 1 and fail due to queueing being
    // disabled.
    assert.soon(() => {
        try {
            new Mongo(`mongodb://${nonExemptIP}:${ingressPort}`);
        } catch (e) {
            return e.message.includes("Connection closed by peer");
        }
        return false;
    });

    proxy_server.stop();
}

rs.stopSet();
