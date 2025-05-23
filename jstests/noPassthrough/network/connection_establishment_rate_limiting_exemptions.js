/**
 * Tests for the ingressConnectionEstablishment rate limiter IP-based exemptions.
 *
 * The ip-based exemptions tests are complicated by the gRPC testing logic, and so it is
 * excluded for now.
 * @tags: [
 *      grpc_incompatible,
 * ]
 */

import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {
    getConnectionStats,
    runTestReplSet,
    runTestShardedCluster,
    runTestStandaloneParamsSetAtRuntime,
    runTestStandaloneParamsSetAtStartup
} from "jstests/noPassthrough/network/libs/conn_establishment_rate_limiter_helpers.js";

const nonExemptIP = get_ipaddr();
const exemptIP = "127.0.0.1";

const testExemptIPsFromRateLimit = (conn) => {
    // Make one connection over the public ip that will consume a token.
    assert(new Mongo(`mongodb://${nonExemptIP}:${conn.port}`));

    // The refreshRate is 1 conn/second, and so these connection attempts will outpace the
    // refreshRate and fail because queue depth is 0.
    assert.soon(() => {
        try {
            new Mongo(`mongodb://${nonExemptIP}:${conn.port}`);
        } catch (e) {
            jsTestLog(e);
            return e.message.includes("Connection closed by peer") ||
                e.message.includes("Connection reset by peer") ||
                e.message.includes("established connection was aborted");
        }

        return false;
    });

    assert.eq(1, getConnectionStats(conn)["establishmentRateLimit"]["totalRejected"]);

    // Connections over an exempted ip will succeed.
    assert(new Mongo(`mongodb://${exemptIP}:${conn.port}`));
    assert.gte(getConnectionStats(conn)["establishmentRateLimit"]["totalExempted"], 1);
};

const testExemptIPsFromRateLimitOpts = {
    ingressConnectionEstablishmentRatePerSec: 1,
    ingressConnectionEstablishmentBurstSize: 1,
    ingressConnectionEstablishmentMaxQueueDepth: 0,
    maxEstablishingConnectionsOverride: {ranges: [exemptIP]},
};
runTestStandaloneParamsSetAtStartup(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestStandaloneParamsSetAtRuntime(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestReplSet(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestShardedCluster(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
