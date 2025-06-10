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
                e.message.includes("established connection was aborted") ||
                e.message.includes("Broken pipe");
        }

        return false;
    });

    assert.soon(() => 1 == getConnectionStats(conn)["establishmentRateLimit"]["rejected"]);

    // Connections over an exempted ip will succeed.
    assert(new Mongo(`mongodb://${exemptIP}:${conn.port}`));
    assert.soon(() => getConnectionStats(conn)["establishmentRateLimit"]["exempted"] >= 1);
};

const testExemptIPsFromRateLimitOpts = {
    ingressConnectionEstablishmentRateLimiterEnabled: true,
    ingressConnectionEstablishmentRatePerSec: 1,
    ingressConnectionEstablishmentBurstCapacitySecs: 1,
    ingressConnectionEstablishmentMaxQueueDepth: 0,
    ingressConnectionEstablishmentRateLimiterBypass: {ranges: [exemptIP]},
};
runTestStandaloneParamsSetAtStartup(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestStandaloneParamsSetAtRuntime(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestReplSet(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestShardedCluster(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
