/**
 * Tests for the ingressRequestAdmissionMaxQueueDepth server parameter and end-to-end queuing
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
    withForcedQueueing,
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
        exemptConn.adminCommand({
            setParameter: 1,
            ingressRequestAdmissionBurstCapacitySecs: kBurstOneSecs,
        }),
    );
}

/** Enables the fractional-rate failpoint override for deterministic slow-rate queuing. */
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

function getQueuedOpsWithComment(exemptConn, comment) {
    return exemptConn
        .getDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, localOps: true}},
            {$match: {"command.comment": comment, "currentQueue.name": "ingress"}},
        ])
        .toArray();
}

function countQueuedOpsWithComment(exemptConn, comment) {
    return getQueuedOpsWithComment(exemptConn, comment).length;
}

function waitForQueuedOp(exemptConn, comment) {
    let queuedOp;
    assert.soonRetryOnNetworkErrors(
        () => {
            const ops = getQueuedOpsWithComment(exemptConn, comment);
            if (ops.length === 0) {
                return false;
            }
            queuedOp = ops[0];
            return true;
        },
        "expected the queued request to appear in $currentOp with currentQueue.name == 'ingress'",
        30000,
        200,
    );
    return queuedOp;
}

/** Kills all queued ops and waits for the ingress queue to fully drain. */
function killQueuedOpsAndWaitForDrain(exemptConn, beforeStats) {
    assert.soonRetryOnNetworkErrors(
        () => {
            assert.commandWorked(exemptConn.adminCommand({killAllSessions: []}));
            // Also kill any no-lsid ops stuck in the rate limiter on the local node (e.g.
            // endSessions commands issued by the JS GC finalizer). These ops have no lsid so
            // killAllSessions cannot reach them, and they sleep indefinitely at the slow rate.
            const localOps = exemptConn
                .getDB("admin")
                .aggregate([{$currentOp: {allUsers: true, localOps: true}}])
                .toArray();
            for (const op of localOps) {
                if (!op.lsid && op.opid && op.command && op.command.endSessions) {
                    exemptConn.adminCommand({killOp: 1, op: op.opid});
                }
            }
            const stats = getRateLimiterStats(exemptConn);
            return (
                stats.addedToQueue - beforeStats.addedToQueue ===
                stats.removedFromQueue - beforeStats.removedFromQueue
            );
        },
        "timed out waiting for queued ingress requests to drain after killAllSessions",
        30000,
        200,
    );
}

function testCurrentOpAndServerStatusReportIngressQueue(conn, exemptConn) {
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 5}),
    );

    const before = getRateLimiterStats(exemptConn);
    const beforeServerStatus = exemptConn.getDB("admin").serverStatus();
    const beforeQueuesIngress = beforeServerStatus.queues
        ? beforeServerStatus.queues.ingress
        : undefined;

    const kComment = "testCurrentOpAndServerStatusReportIngressQueue";
    const t = new Thread(
        async function (host, maxTimeMS, comment) {
            const {makeAuthConn} = await import(
                "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
            );
            const c = makeAuthConn(host);
            return c.getDB("test").runCommand({find: "col", filter: {}, maxTimeMS, comment});
        },
        conn.host,
        kQueuedFindMaxTimeMS,
        kComment,
    );

    withForcedQueueing(exemptConn, () => {
        // While hangInRateLimiter is active, the thread's find is guaranteed to take the queueing
        // path so we can deterministically observe it in $currentOp and in serverStatus.
        t.start();

        // (1) $currentOp must surface the queued op as being in the "ingress" queue and report
        // operation-level queue-time stats under queues.ingress.
        const queuedOp = waitForQueuedOp(exemptConn, kComment);
        assert.eq(
            queuedOp.currentQueue.name,
            "ingress",
            "queued op should report 'ingress' as currentQueue.name",
            {
                op: queuedOp,
            },
        );
        assert(
            queuedOp.queues && queuedOp.queues.ingress,
            "queued op should include queues.ingress metrics",
            {
                op: queuedOp,
            },
        );
        assert.gte(
            queuedOp.queues.ingress.totalTimeQueuedMicros,
            queuedOp.currentQueue.timeQueuedMicros,
            "queues.ingress.totalTimeQueuedMicros should include at least currentQueue.timeQueuedMicros",
            {op: queuedOp},
        );

        // Queue time should be observed as progressing while the op remains queued.
        const initialQueuedMicros = queuedOp.currentQueue.timeQueuedMicros;
        assert.soonRetryOnNetworkErrors(
            () => {
                const ops = getQueuedOpsWithComment(exemptConn, kComment);
                if (ops.length === 0) {
                    return false;
                }
                return ops[0].currentQueue.timeQueuedMicros > initialQueuedMicros;
            },
            "expected queued op currentQueue.timeQueuedMicros to increase while queued",
            30000,
            200,
        );

        // (2) serverStatus.network.ingressRequestRateLimiter should reflect non-regressed general
        // queueing counters for IRRL.
        const after = getRateLimiterStats(exemptConn);
        assert.gte(
            after.addedToQueue - before.addedToQueue,
            1,
            "serverStatus should report the queued request via ingressRequestRateLimiter.addedToQueue",
            {before, after},
        );
        assert.gte(
            after.attemptedAdmissions - before.attemptedAdmissions,
            1,
            "expected at least one attempted admission for the queued request",
            {before, after},
        );
        assert.gte(
            after.attemptedAdmissions - before.attemptedAdmissions,
            after.addedToQueue - before.addedToQueue,
            "attempted admissions should be at least the number of queued admissions",
            {before, after},
        );
        assert.eq(
            after.rejectedAdmissions - before.rejectedAdmissions,
            0,
            "queued request should not be rejected while maxQueueDepth has headroom",
            {before, after},
        );

        // (3) serverStatus.queues.ingress should keep exposing ingress queue stats fields.
        const statusWithQueuedOp = exemptConn.getDB("admin").serverStatus();

        const queuesIngress = statusWithQueuedOp.queues
            ? statusWithQueuedOp.queues.ingress
            : undefined;
        assert.neq(queuesIngress, undefined, "expected serverStatus.queues.ingress", {
            queues: statusWithQueuedOp.queues,
        });
        assert(
            queuesIngress.normalPriority,
            "serverStatus.queues.ingress should include normalPriority stats",
            {
                queuesIngress,
            },
        );
        const normalPriorityStats = queuesIngress.normalPriority;

        assert.neq(beforeQueuesIngress, undefined, "expected baseline serverStatus.queues.ingress");
        assert(
            beforeQueuesIngress.normalPriority,
            "expected baseline serverStatus.queues.ingress.normalPriority",
            {
                beforeQueuesIngress,
            },
        );
        assert(
            normalPriorityStats.hasOwnProperty("totalTimeQueuedMicros"),
            "expected serverStatus.queues.ingress.normalPriority.totalTimeQueuedMicros",
            {normalPriorityStats},
        );
        assert(
            beforeQueuesIngress.normalPriority.hasOwnProperty("totalTimeQueuedMicros"),
            "expected baseline serverStatus.queues.ingress.normalPriority.totalTimeQueuedMicros",
            {beforeQueuesIngress},
        );
        assert.gte(
            normalPriorityStats.totalTimeQueuedMicros,
            beforeQueuesIngress.normalPriority.totalTimeQueuedMicros,
            "expected non-decreasing serverStatus.queues.ingress.normalPriority.totalTimeQueuedMicros",
            {beforeQueuesIngress, queuesIngress},
        );
    });

    withRateLimitingDisabled(exemptConn, () => {
        killQueuedOpsAndWaitForDrain(exemptConn, before);
        t.join();
    });

    // Post-drain invariant: queued requests have matching add/remove accounting.
    const drained = getRateLimiterStats(exemptConn);
    assert.eq(
        drained.addedToQueue - before.addedToQueue,
        drained.removedFromQueue - before.removedFromQueue,
        "expected queued requests to be fully removed after cleanup",
        {before, drained},
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
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 0}),
    );
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
    assert.eq(
        statsDelta.addedToQueue,
        0,
        "no requests should enter the queue when maxQueueDepth=0",
    );
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
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 20}),
    );

    // Disable the slow-rate failpoint that the helper applies at startup.
    disableFractionalRateOverride(exemptConn);

    // 10 req/sec with burst=1: 5 concurrent threads each making findOne requests will reliably end up queuing.
    // Set burstCapacitySecs first so the intermediate state still has burst >= 1.
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionBurstCapacitySecs: 0.1}),
    );
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionRatePerSec: 10}),
    );

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
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 2}),
    );

    const before = getRateLimiterStats(exemptConn);

    const kComment = "testQueueAtCapacityRejectsExcess";
    const numThreads = 5;
    const threads = [];
    for (let i = 0; i < numThreads; i++) {
        const t = new Thread(
            async function (host, maxTimeMS, comment) {
                const {makeAuthConn} = await import(
                    "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
                );
                const authConn = makeAuthConn(host);
                const res = authConn
                    .getDB("test")
                    .runCommand({find: "col", filter: {}, maxTimeMS, comment});
                if (!res.ok) {
                    // Requests rejected at queue capacity get IngressRequestRateLimitExceeded;
                    // requests that queued and were then killed (by the end of test drain) get
                    // Interrupted.
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
            kComment,
        );
        threads.push(t);
    }

    withForcedQueueing(exemptConn, () => {
        // hangInRateLimiter forces every admit attempt through the queueing path; with
        // maxQueueDepth=2, the first two attempts enqueue and the next three hit the
        // queue-full check in RateLimiter::_impl->enqueue() and get IngressRequestRateLimitExceeded.
        for (const t of threads) {
            t.start();
        }

        // Wait until exactly maxQueueDepth of our ops are queued and the rest have been rejected.
        assert.soonRetryOnNetworkErrors(
            () => {
                const numQueuedOps = countQueuedOpsWithComment(exemptConn, kComment);
                const s = getRateLimiterStats(exemptConn);
                return numQueuedOps == 2 && s.rejectedAdmissions - before.rejectedAdmissions >= 3;
            },
            "expected queue to fill and excess requests to be rejected",
            30000,
            200,
        );

        const after = getRateLimiterStats(exemptConn);
        assert.lte(
            after.addedToQueue - before.addedToQueue,
            2,
            "at most maxQueueDepth requests should have been enqueued",
        );
        assert.gte(
            after.rejectedAdmissions - before.rejectedAdmissions,
            1,
            "at least one request should be rejected when the queue is full",
        );
    });

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
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 10}),
    );

    const before = getRateLimiterStats(exemptConn);

    const kComment = "testInterruptedQueuedRequestsIncrementCounter";
    const numThreads = 3;
    const threads = [];
    for (let i = 0; i < numThreads; i++) {
        const t = new Thread(
            async function (host, maxTimeMS, comment) {
                const {makeAuthConn} = await import(
                    "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
                );
                const c = makeAuthConn(host);
                // This command will queue; we expect it to be killed.
                return c.getDB("test").runCommand({find: "col", filter: {}, maxTimeMS, comment});
            },
            conn.host,
            kQueuedFindMaxTimeMS,
            kComment,
        );
        threads.push(t);
    }

    withForcedQueueing(exemptConn, () => {
        for (const t of threads) {
            t.start();
        }

        // Wait until all threads have a request visible in currentOp before killing, so that each
        // one is counted in interruptedInQueue.
        assert.soonRetryOnNetworkErrors(
            () => countQueuedOpsWithComment(exemptConn, kComment) >= numThreads,
            "expected all threads to have a request in the queue",
            30000,
            200,
        );

        // Interrupt all queued operations, then wait until none of our tagged ops remain in
        // currentOp.
        assert.commandWorked(exemptConn.adminCommand({killAllSessions: []}));
        assert.soonRetryOnNetworkErrors(
            () => countQueuedOpsWithComment(exemptConn, kComment) === 0,
            "expected all queued threads to disappear from currentOp after killAllSessions",
            30000,
            200,
        );
    });

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
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 0}),
    );
    forceSlowRateBurstOne(exemptConn);

    const conn2 = makeAuthConn(conn.host);

    // Consume burst then verify rejection with queue disabled.
    assert.commandWorked(conn2.getDB("test").runCommand({find: "col", filter: {}}));
    assert.commandFailedWithCode(
        conn2.getDB("test").runCommand({find: "col", filter: {}}),
        ErrorCodes.IngressRequestRateLimitExceeded,
        "should reject when queue disabled",
    );

    // Enable queuing.
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 5}),
    );

    const before = getRateLimiterStats(exemptConn);

    // Now a request that would have been rejected should queue instead.
    const kComment = "testDynamicQueueDepthUpdate";
    const t = new Thread(
        async function (host, maxTimeMS, comment) {
            const {makeAuthConn} = await import(
                "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
            );
            const c = makeAuthConn(host);
            return c.getDB("test").runCommand({find: "col", filter: {}, maxTimeMS, comment});
        },
        conn.host,
        kQueuedFindMaxTimeMS,
        kComment,
    );
    t.start();

    assert.soonRetryOnNetworkErrors(
        () => countQueuedOpsWithComment(exemptConn, kComment) >= 1,
        "expected request to enter queue after maxQueueDepth was increased",
        30000,
        200,
    );

    withRateLimitingDisabled(exemptConn, () => {
        killQueuedOpsAndWaitForDrain(exemptConn, before);
        t.join();
    });

    // Disable queuing again.
    assert.commandWorked(
        exemptConn.adminCommand({setParameter: 1, ingressRequestAdmissionMaxQueueDepth: 0}),
    );

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
    testCurrentOpAndServerStatusReportIngressQueue,
    testMaxQueueDepthParameterValidation,
    testQueueDisabledRejectsImmediately,
    testConcurrentRequestsQueueAndSucceed,
    testQueueAtCapacityRejectsExcess,
    testInterruptedQueuedRequestsIncrementCounter,
    testDynamicQueueDepthUpdate,
];

for (const {name, runner} of kTopologies) {
    jsTest.log(`Running queuing suite on topology: ${name}`);
    runner({startupParams: kSlowRateBurstOneParams, auth: true}, (conn, exemptConn) => {
        for (const fn of kTests) {
            jsTest.log(`Running ${fn.name} on topology: ${name}`);
            resetTestState(exemptConn);
            fn(conn, exemptConn);
        }
    });
}
