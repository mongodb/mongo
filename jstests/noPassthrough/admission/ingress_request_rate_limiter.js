
/**
 * Test that the ingress request rate limiter works correctly and exposes the right metrics.
 * @tags: [requires_fcv_80]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    authenticateConnection,
    getRateLimiterStats,
    kConfigLogsAndFailPointsForRateLimiterTests,
    kRateLimiterExemptAppName,
    kSlowestRefreshRateSecs,
    runTestReplSet,
    runTestSharded,
    runTestStandalone,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

/**
 * Runs the set parameter commands with some arbitrary value to ensure invalid values are rejected.
 */
function testServerParameter(conn) {
    const db = conn.getDB("admin");
    // Test setting burst capacity.
    assert.commandFailedWithCode(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionBurstCapacitySecs: -1,
    }),
                                 ErrorCodes.BadValue);

    assert.commandWorked(
        db.adminCommand({setParameter: 1, ingressRequestAdmissionBurstCapacitySecs: 1}));

    assert.commandWorked(
        db.adminCommand({setParameter: 1, ingressRequestAdmissionBurstCapacitySecs: 2}));

    // Test setting admission rate

    assert.commandFailedWithCode(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionRatePerSec: -1,
    }),
                                 ErrorCodes.BadValue);

    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionRatePerSec: 1,
    }));
}

// Arbitrary but low burst capacity in the amount of commands allowed to pass through.
const maxBurstRequests = 3;
const maxBurstCapacitySecs = maxBurstRequests / kSlowestRefreshRateSecs;

// Since the rate limiter is set to a very low rate and a burst of 3,
// the 3 additional extra will be rejected and will result in an error.
const extraRequests = 3;

/**
 * This function compares the metrics before a test and after a test to ensure they are consistent.
 *
 * This function was factored out since some tests other than testRateLimiterMetrics are doing
 * the same checks.
 */
function assertMetrics(
    initialStatus, finalStatus, expectedAmountOfSuccess, expectedAmountOfFailures) {
    const initialIngressRequestRateLimiter = initialStatus.network.ingressRequestRateLimiter;
    const initialInserts = initialStatus.metrics.commands.insert.total;

    const ingressRequestRateLimiter = finalStatus.network.ingressRequestRateLimiter;
    const inserts = finalStatus.metrics.commands.insert.total;

    assert.eq(ingressRequestRateLimiter.successfulAdmissions -
                  initialIngressRequestRateLimiter.successfulAdmissions,
              expectedAmountOfSuccess);
    assert.eq(ingressRequestRateLimiter.rejectedAdmissions -
                  initialIngressRequestRateLimiter.rejectedAdmissions,
              expectedAmountOfFailures);
    assert.eq(ingressRequestRateLimiter.attemptedAdmissions -
                  initialIngressRequestRateLimiter.attemptedAdmissions,
              expectedAmountOfSuccess + expectedAmountOfFailures);
    assert.eq(inserts - initialInserts, expectedAmountOfSuccess);

    // We also ensure all token have been consumed
    assert.eq(Math.floor(ingressRequestRateLimiter.totalAvailableTokens), 0);
}

/**
 * Runs a fixed amount of operations and verifies the difference in the metrics.
 *
 * It also verifies that the errors has the right error label.
 */
function testRateLimiterMetrics(conn, exemptConn) {
    const db = exemptConn.getDB(`${jsTest.name()}_db`);
    const initialStatus = db.serverStatus();

    // Here we calculate the amount of requests that are expected to be successful using the
    // available amount of token in the rate limiter
    const requestAmount = maxBurstRequests + extraRequests;
    const expectedAmountOfSuccess =
        Math.floor(initialStatus.network.ingressRequestRateLimiter.totalAvailableTokens);
    const expectedAmountOfFailures = requestAmount - expectedAmountOfSuccess;

    // We want to ensure we do at least one successful request and one failed request
    assert.gt(expectedAmountOfSuccess, 0);
    assert.gt(expectedAmountOfFailures, 0);

    // We run inserts command that will either pass or fail. When it fails, we validate the error
    // code and label.
    for (let i = 0; i < requestAmount; ++i) {
        const assertContainSystemOverloadedErrorLabel = (res) => {
            assert(res.hasOwnProperty("errorLabels"), res);
            assert.sameMembers(["SystemOverloadedError"], res.errorLabels);
        };

        const collName = `${jsTest.name()}_coll`;

        const db = conn.getDB(`${jsTest.name()}_db`);
        const result = db.runCommand({insert: collName, documents: [{dummy: 1}]});

        if (result.ok === 0) {
            assert.commandFailedWithCode(result, ErrorCodes.RateLimitExceeded);
            assertContainSystemOverloadedErrorLabel(result);
        }
    }

    const status = db.serverStatus();
    assertMetrics(initialStatus, status, expectedAmountOfSuccess, expectedAmountOfFailures);
}

/**
 * Runs some ping command with an unauthenticated client and validates that all requests were
 * exempt. This test only work on server with authentication enabled.
 *
 * As unauthenticated client are exempt from rate limiting, no token should be consumed.
 */
function testRateLimiterUnauthenticated(noAuthClient, exemptConn) {
    // We run more pings than the available burst size. As all pings are exempts, they should
    // all succeed.
    const amountOfPing = maxBurstRequests + extraRequests;
    const initialRateLimiterStats = getRateLimiterStats(exemptConn);
    const initialExempt = initialRateLimiterStats.exemptedAdmissions;
    const initialAvailableTokens = initialRateLimiterStats.totalAvailableTokens;

    // Run ping on unauthenticated client. As the client is unauthenticated, all command
    // should work as there is no rate limiting applied.
    const noAuthDB = noAuthClient.getDB("admin");
    for (let i = 0; i < maxBurstRequests + extraRequests; ++i) {
        assert.commandWorked(noAuthDB.runCommand({ping: 1}));
    }

    const finalRateLimiterStats = getRateLimiterStats(exemptConn);
    const finalExempt = finalRateLimiterStats.exemptedAdmissions;
    const finalAvailableTokens = finalRateLimiterStats.totalAvailableTokens;

    // Check that we added as much exempt requests as the amount of ping we ran. As no requests
    // should be rate limited, running a command should only increment the exempt counter.
    assert.eq(finalExempt - initialExempt, amountOfPing);

    // Check that we didn't affect the amount of available tokens while running pings.
    assert.eq(finalAvailableTokens - initialAvailableTokens, 0);
}

// Parameters for ingress admission rate limiting enabled.
const kParams = {
    ingressRequestAdmissionRatePerSec: 1,
    ingressRequestAdmissionBurstCapacitySecs: maxBurstCapacitySecs,
    ingressRequestRateLimiterEnabled: true,
};

/**
 * Runs the server parameter test using a standalone instance.
 *
 * This test primarily checks if server parameters are validated correctly.
 *
 * See 'testServerParameter' for more detail.
 */
runTestStandalone({startupParams: {}, auth: true}, (conn, exemptConn) => {
    testServerParameter(exemptConn);
});

/**
 * Runs a test using a standalone instance where we set parameters at startup
 *
 * It also tests that unauthenticated connections are exempt.
 */
runTestStandalone({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    testRateLimiterUnauthenticated(conn, exemptConn);

    authenticateConnection(conn);
    testRateLimiterMetrics(conn, exemptConn);
});

/**
 * Runs the test using a standalone instance where we set parameters at runtime.
 *
 * It also tests that unauthenticated connections are exempt.
 */
runTestStandalone({startupParams: {}, auth: true}, (conn, exemptConn) => {
    // We test setting parameters at runtime.
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParams}));

    authenticateConnection(conn);
    testRateLimiterMetrics(conn, exemptConn);
});

/**
 * Runs the test using a standalone instance where authentication is disabled on the server.
 *
 * The second parameter is set to false in order to disabled the authentication.
 *
 * We verify that unauthenticated clients are not exempt in this case. We do this by skipping
 * the call to authenticateConnection we see in other tests.
 */
runTestStandalone({startupParams: kParams, auth: false}, testRateLimiterMetrics);

/**
 * Runs the test using a standalone instance where we set parameters at startup.
 *
 * It also tests that unauthenticated connections are exempt.
 */
runTestStandalone({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    testRateLimiterUnauthenticated(conn, exemptConn);

    authenticateConnection(conn);
    testRateLimiterMetrics(conn, exemptConn);
});

runTestReplSet({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    testRateLimiterUnauthenticated(conn, exemptConn);

    authenticateConnection(conn);
    testRateLimiterMetrics(conn, exemptConn);
});

runTestSharded({startupParams: kParams, auth: true}, (conn, exemptConn) => {
    testRateLimiterUnauthenticated(conn, exemptConn);

    authenticateConnection(conn);
    testRateLimiterMetrics(conn, exemptConn);
});

/**
 * This function tests the ingress admission rate limiter when with a compressed client.
 *
 * This test is different than the others because for compression to work a parallelShell is
 * needed.
 */
function runTestCompressed() {
    const kCompressor = "snappy";
    const mongod = MongoRunner.runMongod({
        networkMessageCompressors: kCompressor,
        setParameter: {
            ...kParams,
            ...kConfigLogsAndFailPointsForRateLimiterTests,
            ingressRequestRateLimiterEnabled: false,
        },
    });

    // We create an exempt connection in order to get the status without affecting the metrics
    const compressedConn = new Mongo(`mongodb://${mongod.host}/?compressors=${kCompressor}`);
    const exemptConn = new Mongo(`mongodb://${mongod.host}/?appName=${kRateLimiterExemptAppName}`);
    const admin = exemptConn.getDB("admin");

    // We calculate the amount of expected success and failures depending on the amount of token
    // available
    const initialStatus = admin.serverStatus();
    const initialAvailableTokens =
        initialStatus.network.ingressRequestRateLimiter.totalAvailableTokens;

    const requestAmount = maxBurstRequests + extraRequests;
    const expectedAmountOfSuccess =
        Math.floor(initialStatus.network.ingressRequestRateLimiter.totalAvailableTokens);
    const expectedAmountOfFailures = requestAmount - expectedAmountOfSuccess;

    // Here we run a parallel shell so we can enable network compression
    const compressionArgs = ["--networkMessageCompressors", kCompressor];
    const shell = startParallelShell(
        funWithArgs(
            (host, params, exemptAppName, requestAmount) => {
                const exemptConn = new Mongo(`mongodb://${host}/?appName=${exemptAppName}`);
                exemptConn.getDB("admin").auth("admin", "pwd");
                assert.commandWorked(
                    exemptConn.adminCommand({setParameter: 1, ...JSON.parse(params)}));

                // We run inserts command that will either pass or fail. When it fails, we validate
                // the error code and label
                for (let i = 0; i < requestAmount; ++i) {
                    const assertContainSystemOverloadedErrorLabel = (res) => {
                        assert(res.hasOwnProperty("errorLabels"), res);
                        assert.sameMembers(["SystemOverloadedError"], res.errorLabels);
                    };

                    const collName = `${jsTest.name()}_coll`;

                    const result = db.runCommand({insert: collName, documents: [{dummy: 1}]});

                    if (result.ok === 0) {
                        assert.commandFailedWithCode(result, ErrorCodes.RateLimitExceeded);
                        assertContainSystemOverloadedErrorLabel(result);
                    }
                }
                assert.commandWorked(exemptConn.adminCommand(
                    {setParameter: 1, ingressRequestRateLimiterEnabled: 0}));
            },
            mongod.host,
            JSON.stringify(kParams),
            kRateLimiterExemptAppName,
            requestAmount),
        mongod.port,
        false,
        ...compressionArgs);

    shell();

    // We get the final metrics so we can compare and assert on the difference
    const finalStatus = admin.serverStatus();
    const finalExempt = finalStatus.network.ingressRequestRateLimiter.exemptedAdmissions;
    const finalAvailableTokens = finalStatus.network.ingressRequestRateLimiter.totalAvailableTokens;

    assertMetrics(initialStatus, finalStatus, expectedAmountOfSuccess, expectedAmountOfFailures);

    MongoRunner.stopMongod(mongod);
}

runTestCompressed();
