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
    getLimiterStats,
    runTestReplSet,
    runTestShardedCluster,
    runTestStandaloneParamsSetAtRuntime,
    runTestStandaloneParamsSetAtStartup
} from "jstests/noPassthrough/network/libs/conn_establishment_rate_limiter_helpers.js";

// Return whether the left Number or NumberLong is equal to the right Number or NumberLong.
const equal = (left, right) => {
    if (left instanceof NumberLong && right instanceof NumberLong) {
        return left.compare(right) === 0;
    }
    return left == right;
};

const maxQueueSize = 3;
// Hardcoded so that we can assert on "available" connections in serverStatus.
const maxIncomingConnections = 1000;

const testRateLimiterStats = (conn) => {
    // Start maxQueueSize + 3 threads that will all try to connect to the server. The rate limiter
    // should allow maxQueueSize connections to be queued, and the rest should be rejected.
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
        const {connections: cstats, ingressSessionEstablishmentQueues: qstats} =
            getLimiterStats(conn, {log: false});

        jsTestLog("stats: " + tojson({cstats, qstats}));

        const checks = [
            // Queued connections should be counted in "queuedForEstablishment", "totalCreated", and
            // "available" stats.
            () => equal(cstats["queuedForEstablishment"], maxQueueSize),
            () => cstats["totalCreated"] >= maxQueueSize,
            () => cstats["available"] <= maxIncomingConnections - maxQueueSize,
            // "extraConns" connections should be rejected because we create more than the rate
            // limit can handle-- they should be counted as rejected in the establishmentRateLimit
            // subsection and the overall "rejected" count.
            () => equal(cstats["rejected"], extraConns),
            () => equal(cstats["establishmentRateLimit"]["rejected"], extraConns),
            // There is a correspondence between the connections stats and the session
            // establishment queue stats, because they use the same underlying rate limiter.
            () => equal(cstats["establishmentRateLimit"]["rejected"], qstats["rejectedAdmissions"]),
            () => equal(cstats["queuedForEstablishment"],
                        qstats["addedToQueue"] - qstats["removedFromQueue"]),
            // Somebody either waited or was admitted immediately, so there is an average wait
            // time.
            () => qstats["averageTimeQueuedMicros"] >= 0
        ];

        // results :: {<body of check>: <boolean result of check>}
        const results = Object.fromEntries(checks.map(check => [check.toString(), check()]));

        jsTestLog("checks: " + tojson(results));

        // "all of the checks passed"
        return Object.values(results).every(result => result);
    });

    connDelayFailPoint.off();

    // Join all background threads before exiting the test.
    for (let i = 0; i < threads.length; ++i) {
        threads[i].join();
    }
};

const testRateLimiterStatsOpts = {
    ingressConnectionEstablishmentRateLimiterEnabled: true,
    ingressConnectionEstablishmentRatePerSec: 1,
    ingressConnectionEstablishmentBurstSize: 1,
    ingressConnectionEstablishmentMaxQueueDepth: maxQueueSize,
};
runTestStandaloneParamsSetAtStartup(testRateLimiterStatsOpts, testRateLimiterStats);
runTestStandaloneParamsSetAtRuntime(testRateLimiterStatsOpts, testRateLimiterStats);
runTestReplSet(testRateLimiterStatsOpts, testRateLimiterStats);
runTestShardedCluster(testRateLimiterStatsOpts, testRateLimiterStats);
