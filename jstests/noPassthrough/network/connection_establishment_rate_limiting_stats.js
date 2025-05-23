/**
 * Tests for the ingressConnectionEstablishment rate limiter metrics.
 *
 * gRPC outputs different metrics from the metrics we assert on here.
 * @tags: [
 *      grpc_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {
    getConnectionStats,
    runTestReplSet,
    runTestShardedCluster,
    runTestStandaloneParamsSetAtRuntime,
    runTestStandaloneParamsSetAtStartup
} from "jstests/noPassthrough/network/libs/conn_establishment_rate_limiter_helpers.js";

const maxQueueSize = 3;
// Hardcoded so that we can assert on "available" connections in serverStatus.
const maxIncomingConnections = 1000;

const testRateLimiterStats = (conn) => {
    // Start maxQueueDepth + 3 threads that will all try to connect to the server. The rate limiter
    // should allow maxQueueDepth connections to be queued, and the rest should be rejected.
    let connDelayFailPoint = configureFailPoint(conn, 'hangInRateLimiter');
    const extraConns = 3;
    const threads = [];
    for (let i = 0; i < maxQueueSize + extraConns; i++) {
        threads.push(new Thread((host, threadId) => {
            try {
                jsTestLog("Thread " + threadId + " trying to connect to " + host);
                // Set a socket timeout so that these threads can eventually be joined.
                new Mongo(`mongodb://${host}/?socketTimeoutMS=8000`);
            } catch (e) {
                jsTestLog("Thread " + threadId + " caught error: " + e);
            }
        }, conn.host, i));
        threads[i].start();
    }

    assert.soon(() => {
        const stats = getConnectionStats(conn);
        // Queued connections should be counted in "queued", "totalCreated", and "available" stats.
        return (stats["queued"] == maxQueueSize && stats["totalCreated"] >= maxQueueSize &&
                stats["available"] <= maxIncomingConnections - maxQueueSize) &&
            // "extraConns" connections should be rejected because we create more than the rate
            // limit can handle-- they should be counted as rejected in the establishmentRateLimit
            // subsection and the overall "rejected" count.
            (stats["rejected"] == extraConns &&
             stats["establishmentRateLimit"]["totalRejected"] == extraConns);
    });

    connDelayFailPoint.off();

    // Join all background threads before exiting the test.
    for (let i = 0; i < threads.length; ++i) {
        threads[i].join();
    }
};

const testRateLimiterStatsOpts = {
    ingressConnectionEstablishmentRatePerSec: 1,
    ingressConnectionEstablishmentBurstSize: 1,
    ingressConnectionEstablishmentMaxQueueDepth: maxQueueSize,
};
runTestStandaloneParamsSetAtStartup(testRateLimiterStatsOpts, testRateLimiterStats);
runTestStandaloneParamsSetAtRuntime(testRateLimiterStatsOpts, testRateLimiterStats);
runTestReplSet(testRateLimiterStatsOpts, testRateLimiterStats);
runTestShardedCluster(testRateLimiterStatsOpts, testRateLimiterStats);
