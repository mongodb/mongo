/**
 * Tests for the ingress request rate limiter IP-based exemptions.
 * @tags: [requires_fcv_80]
 */

import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {
    authenticateConnection,
    getRateLimiterStats,
    runTestReplSet,
    runTestSharded,
    runTestStandalone,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const testExemptIPsFromRateLimit = (rateExemptConn, exemptClient, nonExemptClient) => {
    // We authenticate so our requests are rate limited
    authenticateConnection(exemptClient);
    authenticateConnection(nonExemptClient);

    const initialRateLimiterStats = getRateLimiterStats(rateExemptConn);

    // Make one ping over the non exempt ip so a token is consumed.
    const nonExemptDb = nonExemptClient.getDB("admin");
    assert.commandWorked(nonExemptDb.runCommand({ping: 1}));

    // The refresh rate is 0.5e-6 requests/second, and so these commands will
    // fail because the burst rate is exhausted.
    assert.commandFailed(nonExemptDb.runCommand({ping: 1}));

    // Requests over an exempted ip will succeed. We run two of them and since we use a very slow
    // rate, the following commands would fail if they were not exempted.
    const exemptDB = exemptClient.getDB("admin");
    assert.commandWorked(exemptDB.runCommand({ping: 1}));
    assert.commandWorked(exemptDB.runCommand({ping: 1}));

    // Verify that the non exempted clients are still failing.
    assert.commandFailed(nonExemptDb.runCommand({ping: 1}));

    const finalRateLimiterStats = getRateLimiterStats(rateExemptConn);

    assert.eq(
        finalRateLimiterStats.successfulAdmissions - initialRateLimiterStats.successfulAdmissions,
        1);
    assert.eq(finalRateLimiterStats.rejectedAdmissions - initialRateLimiterStats.rejectedAdmissions,
              2);
    assert.eq(
        finalRateLimiterStats.attemptedAdmissions - initialRateLimiterStats.attemptedAdmissions, 3);
    assert.eq(finalRateLimiterStats.exemptedAdmissions - initialRateLimiterStats.exemptedAdmissions,
              2);
};

const nonExemptIP = get_ipaddr();
const exemptIP = "127.0.0.1";

const kParams = {
    ingressRequestAdmissionRatePerSec: 1,
    ingressRequestAdmissionBurstSize: 1,
    ingressRequestRateLimiterEnabled: true,
    ingressRequestRateLimiterExemptions: {ranges: [exemptIP]},
};

runTestStandalone({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    const nonExemptClient = new Mongo(`mongodb://${nonExemptIP}:${conn.port}`);
    testExemptIPsFromRateLimit(exemptConn, conn, nonExemptClient);
});

runTestReplSet({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    const nonExemptClient = new Mongo(`mongodb://${nonExemptIP}:${conn.port}`);
    testExemptIPsFromRateLimit(exemptConn, conn, nonExemptClient);
});

runTestSharded({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    const nonExemptClient = new Mongo(`mongodb://${nonExemptIP}:${conn.port}`);
    testExemptIPsFromRateLimit(exemptConn, conn, nonExemptClient);
});

// The tests defined after this line only work on POSIX machines.
if (_isWindows()) {
    quit();
}

runTestStandalone({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    const nonExemptClient = new Mongo(`mongodb://${nonExemptIP}:${conn.port}`);
    const exemptAdmin = exemptConn.getDB("admin");
    const unixPort = `/tmp/mongodb-${conn.port}.sock`;
    assert.commandWorked(exemptAdmin.adminCommand(
        {setParameter: 1, ingressRequestRateLimiterExemptions: {ranges: [unixPort]}}));
    const exemptSocketClient = new Mongo(`mongodb://${encodeURIComponent(unixPort)}`);
    testExemptIPsFromRateLimit(exemptConn, exemptSocketClient, nonExemptClient);
});
