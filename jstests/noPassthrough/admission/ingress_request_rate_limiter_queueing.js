/**
 * Tests for the ingressRequestAdmissionMaxQueueDepth server parameter and end-to-end queueing
 * behavior of the ingress request rate limiter.
 *
 * @tags: [requires_fcv_80]
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {
    getRateLimiterStats,
    kSlowestRefreshRateSecs,
    makeAuthConn,
    measureQueueStats,
    runTestReplSet,
    runTestSharded,
    runTestStandalone,
    withRateLimitingDisabled,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

// The test intentionally sets an extremely slow admission rate, so a queued find can otherwise
// wait for a very long time. Capping the amount of time a queued find can wait prevents the test
// from hanging indefinitely if the killAllSessions command fails. Keep this timeout conservatively
// high for slower TSAN/debug builds.
const kQueuedFindMaxTimeMS = 30000;

const kBurstOneSecs = Math.round(1.0 / kSlowestRefreshRateSecs);
const kSlowRateBurstOneParams = {
    ingressRequestAdmissionBurstCapacitySecs: kBurstOneSecs,
    ingressRequestRateLimiterEnabled: true,
};

/** Forces the rate limiter's token bucket to be re-clamped to burst=1 at the slow rate. */
function forceSlowRateBurstOne(exemptConn) {
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionBurstCapacitySecs: kBurstOneSecs}),
    );
}

/** Enables the fractional-rate failpoint override for deterministic slow-rate queueing. */
function enableFractionalRateOverride(exemptConn, rate) {
    assert.commandWorked(
        exemptConn.adminCommand({
            configureFailPoint: "ingressRequestRateLimiterFractionalRateOverride",
            mode: "alwaysOn",
            data: {rate},
        }),
    );
}

/** Disables the fractional-rate failpoint override to use the configured server rate. */
function disableFractionalRateOverride(exemptConn) {
    assert.commandWorked(
        exemptConn.adminCommand({
            configureFailPoint: "ingressRequestRateLimiterFractionalRateOverride",
            mode: "off",
        }),
    );
}

/** Kills all queued ops and waits for the ingress queue to fully drain. */
function killQueuedOpsAndWaitForDrain(exemptConn, beforeStats) {
    assert.soonRetryOnNetworkErrors(
        () => {
            assert.commandWorked(exemptConn.adminCommand({killAllSessions: []}));
            const stats = getRateLimiterStats(exemptConn);
            return (
                stats.addedToQueue - beforeStats.addedToQueue === stats.removedFromQueue - beforeStats.removedFromQueue
            );
        },
        "timed out waiting for queued ingress requests to drain after killAllSessions",
        30000,
        200,
    );
}

// ---------------------------------------------------------------------------
// Test: ingressRequestAdmissionMaxQueueDepth parameter validation
// ---------------------------------------------------------------------------
function testMaxQueueDepthParameterValidation(conn, exemptConn) {
    assert.commandFailedWithCode(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: -1}),
        ErrorCodes.BadValue,
        "negative queue depth should be rejected",
    );

    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 0}),
        "zero queue depth (disabled) should be accepted",
    );

    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 100}),
        "positive queue depth should be accepted",
    );
}

// ---------------------------------------------------------------------------
// Test: With maxQueueDepth = 0 (default), excess requests are rejected immediately and
//       addedToQueue stays zero.
// ---------------------------------------------------------------------------
function testQueueDisabledRejectsImmediately(conn, exemptConn) {
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 0}));
    forceSlowRateBurstOne(exemptConn);

    // Connection setup happens while the client is unauthenticated and is therefore exempt
    // from the rate limiter, so the multi-command auth handshake completes even with burst=1.
    const conn2 = makeAuthConn(conn.host);
    // Consume the single burst token.
    assert.commandWorked(conn2.getDB("test").runCommand({find: "col", filter: {}}));

    const statsDelta = measureQueueStats(exemptConn, () => {
        assert.commandFailedWithCode(
            conn2.getDB("test").runCommand({find: "col", filter: {}}),
            ErrorCodes.IngressRequestRateLimitExceeded,
            "request should be rejected when queue is disabled",
        );
    });
    assert.eq(statsDelta.addedToQueue, 0, "no requests should enter the queue when maxQueueDepth=0");
    assert.gte(statsDelta.rejectedAdmissions, 1, "rejected count should increment");
}

// ---------------------------------------------------------------------------
// Test: Concurrent requests that exceed burst capacity queue and eventually succeed.
//       Verifies addedToQueue and removedFromQueue metrics are consistent.
//
// Disables the slow-rate failpoint and uses a moderate rate so queued leases drain quickly
// and threads can complete without being killed.
// ---------------------------------------------------------------------------
function testConcurrentRequestsQueueAndSucceed(conn, exemptConn) {
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 20}));

    // Disable the slow-rate failpoint that the helper applies at startup.
    disableFractionalRateOverride(exemptConn);

    // 10 req/sec with burst=1: 5 concurrent threads each making findOne requests will reliably end up queueing.
    // Set burstCapacitySecs first so the intermediate state still has burst >= 1.
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionBurstCapacitySecs: 0.1}));
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionRatePerSec: 10}));

    const numThreads = 5;
    const threads = [];
    for (let i = 0; i < numThreads; i++) {
        const t = new Thread(async function (host) {
            const {makeAuthConn} = await import(
                "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
            );
            const authConn = makeAuthConn(host);
            assert.commandWorked(authConn.getDB("test").runCommand({find: "col", filter: {}}));
        }, conn.host);
        threads.push(t);
    }

    const statsDelta = measureQueueStats(exemptConn, () => {
        for (const t of threads) {
            t.start();
        }
        for (const t of threads) {
            t.join();
        }
    });
    const queued = statsDelta.addedToQueue;
    const dequeued = statsDelta.removedFromQueue;

    assert.gte(queued, 1, "at least some requests should have queued");
    assert.eq(queued, dequeued, "every queued request should have been dequeued on success");
    assert.eq(statsDelta.interruptedInQueue, 0, "no requests should have been interrupted");
}

// ---------------------------------------------------------------------------
// Test: When the queue is at capacity, requests beyond the limit are rejected.
// ---------------------------------------------------------------------------
function testQueueAtCapacityRejectsExcess(conn, exemptConn) {
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 2}));
    forceSlowRateBurstOne(exemptConn);

    const conn2 = makeAuthConn(conn.host);

    // Exhaust the burst token so the first thread request immediately queues.
    assert.commandWorked(conn2.getDB("test").runCommand({find: "col", filter: {}}));

    const before = getRateLimiterStats(exemptConn);

    const numThreads = 5;
    const threads = [];
    for (let i = 0; i < numThreads; i++) {
        const t = new Thread(
            async function (host, maxTimeMS) {
                const {makeAuthConn} = await import(
                    "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
                );
                const authConn = makeAuthConn(host);
                const res = authConn.getDB("test").runCommand({find: "col", filter: {}, maxTimeMS});
                if (!res.ok) {
                    // Requests rejected at queue capacity get IngressRequestRateLimitExceeded;
                    // requests that queued and were then killed (by the end of test drain) get Interrupted.
                    assert.commandFailedWithCode(
                        res,
                        [ErrorCodes.IngressRequestRateLimitExceeded, ErrorCodes.Interrupted],
                        "request should be rejected when queue is at capacity, or interrupted at test cleanup",
                    );
                }

                return res;
            },
            conn.host,
            kQueuedFindMaxTimeMS,
        );
        threads.push(t);
    }
    for (const t of threads) {
        t.start();
    }

    // Wait until stats show the queue is filling up.
    assert.soon(
        () => {
            const s = getRateLimiterStats(exemptConn);
            const expectedRequestsQueued = s.addedToQueue - before.addedToQueue == 2;
            const someRequestsRejected = s.rejectedAdmissions - before.rejectedAdmissions >= 3;
            return expectedRequestsQueued && someRequestsRejected;
        },
        "expected at least one rejection when queue is at capacity",
        10000,
    );

    const after = getRateLimiterStats(exemptConn);
    assert.lte(after.addedToQueue - before.addedToQueue, 2, "at most maxQueueDepth requests should have been enqueued");
    assert.gte(
        after.rejectedAdmissions - before.rejectedAdmissions,
        1,
        "at least one request should be rejected when the queue is full",
    );

    withRateLimitingDisabled(exemptConn, () => {
        killQueuedOpsAndWaitForDrain(exemptConn, before);
        for (const t of threads) {
            t.join();
        }
    });
}

// ---------------------------------------------------------------------------
// Test: Requests interrupted while waiting in the queue increment interruptedInQueue.
// ---------------------------------------------------------------------------
function testInterruptedQueuedRequestsIncrementCounter(conn, exemptConn) {
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 10}));
    forceSlowRateBurstOne(exemptConn);

    const conn2 = makeAuthConn(conn.host);

    // Exhaust the burst token.
    assert.commandWorked(conn2.getDB("test").runCommand({find: "col", filter: {}}));

    const before = getRateLimiterStats(exemptConn);

    // Spawn threads that will queue indefinitely under the slow rate.
    const numThreads = 3;
    const threads = [];
    for (let i = 0; i < numThreads; i++) {
        const t = new Thread(
            async function (host, maxTimeMS) {
                const {makeAuthConn} = await import(
                    "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
                );
                const c = makeAuthConn(host);
                // This command will queue; we expect it to be killed.
                return c.getDB("test").runCommand({find: "col", filter: {}, maxTimeMS});
            },
            conn.host,
            kQueuedFindMaxTimeMS,
        );
        threads.push(t);
    }
    for (const t of threads) {
        t.start();
    }

    // Wait until all threads have a request in the queue before killing, so that each one is
    // counted in interruptedInQueue rather than only a subset.
    assert.soonRetryOnNetworkErrors(
        () => {
            const stats = getRateLimiterStats(exemptConn);
            return stats.addedToQueue - before.addedToQueue >= numThreads;
        },
        "expected all threads to have a request in the queue",
        10000,
        200,
    );

    withRateLimitingDisabled(exemptConn, () => {
        killQueuedOpsAndWaitForDrain(exemptConn, before);
        for (const t of threads) {
            t.join();
        }
    });

    const after = getRateLimiterStats(exemptConn);
    assert.gte(
        after.interruptedInQueue - before.interruptedInQueue,
        numThreads,
        "every thread's queued request should appear in interruptedInQueue",
    );
    assert.eq(
        after.addedToQueue - before.addedToQueue,
        after.removedFromQueue - before.removedFromQueue,
        "every queued request should be dequeued on interruption",
    );
}

// ---------------------------------------------------------------------------
// Test: Dynamic update of ingressRequestAdmissionMaxQueueDepth takes effect immediately.
//       Increasing the queue depth allows more requests to queue; decreasing to 0 causes
//       immediate rejections again.
// ---------------------------------------------------------------------------
function testDynamicQueueDepthUpdate(conn, exemptConn) {
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 0}));
    forceSlowRateBurstOne(exemptConn);

    const conn2 = makeAuthConn(conn.host);

    // Consume burst then verify rejection with queue disabled.
    assert.commandWorked(conn2.getDB("test").runCommand({find: "col", filter: {}}));
    assert.commandFailedWithCode(
        conn2.getDB("test").runCommand({find: "col", filter: {}}),
        ErrorCodes.IngressRequestRateLimitExceeded,
        "should reject when queue disabled",
    );

    // Enable queueing.
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 5}));

    const before = getRateLimiterStats(exemptConn);

    // Now a request that would have been rejected should queue instead.
    const t = new Thread(
        async function (host, maxTimeMS) {
            const {makeAuthConn} = await import(
                "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
            );
            const c = makeAuthConn(host);
            return c.getDB("test").runCommand({find: "col", filter: {}, maxTimeMS});
        },
        conn.host,
        kQueuedFindMaxTimeMS,
    );
    t.start();

    assert.soonRetryOnNetworkErrors(
        () => {
            const stats = getRateLimiterStats(exemptConn);
            return stats.addedToQueue - before.addedToQueue >= 1;
        },
        "expected request to enter queue after maxQueueDepth was increased",
        10000,
        200,
    );

    withRateLimitingDisabled(exemptConn, () => {
        killQueuedOpsAndWaitForDrain(exemptConn, before);
        t.join();
    });

    // Disable queueing again.
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 0}));

    assert.commandFailedWithCode(
        conn2.getDB("test").runCommand({find: "col", filter: {}}),
        ErrorCodes.IngressRequestRateLimitExceeded,
        "should reject again after queue disabled",
    );
}

function resetTestState(exemptConn) {
    const before = getRateLimiterStats(exemptConn);
    withRateLimitingDisabled(exemptConn, () => {
        killQueuedOpsAndWaitForDrain(exemptConn, before);
    });

    // Force at least one token to be available before restoring slow-rate behavior.
    disableFractionalRateOverride(exemptConn);
    assert.commandWorked(
        exemptConn.adminCommand({
            setParameter: 1,
            ingressRequestAdmissionRatePerSec: 1000,
            ingressRequestAdmissionBurstCapacitySecs: 1,
            ingressRequestRateLimiterEnabled: true,
        }),
    );
    assert.soon(
        () => getRateLimiterStats(exemptConn).totalAvailableTokens >= 1,
        "timed out waiting for ingress token refill in test preamble",
        5000,
        100,
    );

    // Restore deterministic baseline used by these tests.
    enableFractionalRateOverride(exemptConn, kSlowestRefreshRateSecs);
    assert.commandWorked(
        exemptConn.adminCommand({
            setParameter: 1,
            ingressRequestAdmissionRatePerSec: 1,
            ingressRequestAdmissionBurstCapacitySecs: kBurstOneSecs,
            ingressRequestAdmissionMaxQueueDepth: 0,
            ingressRequestRateLimiterEnabled: true,
        }),
    );
}

const kTopologies = [
    {name: "standalone", runner: runTestStandalone},
    {name: "replset", runner: runTestReplSet},
    {name: "sharded", runner: runTestSharded},
];

const kTests = [
    testMaxQueueDepthParameterValidation,
    testQueueDisabledRejectsImmediately,
    testConcurrentRequestsQueueAndSucceed,
    testQueueAtCapacityRejectsExcess,
    testInterruptedQueuedRequestsIncrementCounter,
    testDynamicQueueDepthUpdate,
];

for (const {name, runner} of kTopologies) {
    jsTest.log(`Running queueing suite on ${name}`);
    runner({startupParams: kSlowRateBurstOneParams, auth: true}, (conn, exemptConn) => {
        for (const fn of kTests) {
            resetTestState(exemptConn);
            jsTest.log(`Running ${fn.name}`);
            fn(conn, exemptConn);
        }
    });
}
