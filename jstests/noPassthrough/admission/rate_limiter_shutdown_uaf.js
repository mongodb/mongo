/**
 * SERVER-126179 ASAN repro: use-after-free when a request is queued in the
 * rate limiter (sleeping in RateLimiter::acquireToken -> opCtx->sleepUntil) while
 * the server is shut down.
 *
 * Two distinct UAFs can fire on the queued thread when it is woken up by interrupt
 * or by the sleep deadline expiring during shutdown:
 *
 *   (1) The OperationContext is destroyed (its Client is torn down) before the
 *       sleeping thread wakes; the post-wake code path dereferences the freed
 *       opCtx (e.g. when wrapping the interrupt in a Status via e.toStatus()).
 *
 *   (2) The ServiceContext is destroyed before the page-server-reader / flow
 *       control consumer is joined; the queued thread re-enters
 *       opCtx->getServiceContext()->getPreciseClockSource() (or related decorations
 *       held by the rate limiter) after the ServiceContext has been freed.
 *
 * The flow-control rate limiter is the canonical production caller of the
 * blocking acquireToken() entry point, so we wedge a writer there using the
 * hangInRateLimiter failpoint and then shut down.
 *
 * Expected ASAN-clean behavior after fix: mongod exits cleanly with no
 * heap-use-after-free / heap-use-after-scope diagnostic. Current pre-fix
 * behavior: ASAN report on the queued thread or signal handler.
 *
 * @tags: [
 *   requires_flow_control,
 *   requires_replication,
 *   requires_persistence,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {stopReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";

const testName = jsTestName();

const rst = new ReplSetTest({
    name: testName,
    nodes: 3,
    nodeOptions: {
        setParameter: {
            enableFlowControl: true,
            flowControlUseRateLimiter: true,
            flowControlSamplePeriod: 1,
            flowControlTargetLagSeconds: 1,
            flowControlThresholdLagPercentage: 1,
            // Tight queue so any wedged writer is observable in serverStatus.
            flowControlRateLimiterMaxQueueDepth: 64,
            writePeriodicNoops: true,
            periodicNoopIntervalSecs: 1,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(testName);

// Seed the collection so write-path picks the rate-limited admission branch.
assert.commandWorked(primaryDB.c.insert({_id: 0, seed: true}));

// Force flow control into the throttling regime: stop secondary replication so
// the primary is laggy, then engage the failpoint that makes acquireToken sleep
// for ~1h (so the thread is definitively queued at the moment we shut down).
stopReplicationOnSecondaries(rst);

// hangInRateLimiter is defined in admission/rate_limiter.cpp. With it active,
// acquireToken always sees a non-zero napTime and enters opCtx->sleepUntil.
const fp = configureFailPoint(primary, "hangInRateLimiter");

// Kick off a writer in a parallel shell. It will queue inside the flow-control
// rate limiter (no client-side timeout, no maxTimeMS -> the opCtx survives only
// as long as the parent connection / shutdown lets it). We deliberately do NOT
// pass maxTimeMS so the shutdown is the only interrupt source -- that's the
// path that exercises the UAF.
const wedgedWriter = startParallelShell(function () {
    // Best-effort; expected to error/disconnect when the server shuts down.
    try {
        db.getSiblingDB(TestData.testName).c.insert({_id: 1, wedged: true});
    } catch (e) {
        jsTestLog("wedged writer exited: " + e);
    }
}, primary.port);

TestData.testName = testName;

// Wait until the rate limiter has at least one queued request before shutting
// down. queued() is exposed in serverStatus().flowControl.rateLimiter.
assert.soon(
    () => {
        const s = primary.adminCommand({serverStatus: 1});
        const rl = (s && s.flowControl && s.flowControl.rateLimiter) || {};
        // addedToQueue is monotonic; queued() is point-in-time. Either confirms
        // we landed inside opCtx->sleepUntil.
        return (rl.addedToQueue || 0) >= 1;
    },
    "expected at least one writer queued in flow-control rate limiter before shutdown",
    30 * 1000,
    250,
);

jsTestLog("Writer is wedged in RateLimiter::acquireToken; initiating shutdown.");

// Clean shutdown. With the bug present, the queued thread races the
// ServiceContext / OperationContext teardown and ASAN trips on the freed
// memory. With the fix, the queued thread is interrupted cleanly (e.g. via a
// ClientLock + shutdown-aware interrupt source) and the process exits 0.
//
// We use stopSet() rather than SIGKILL because the bug is about *clean*
// shutdown ordering. allowedExitCode is omitted so any non-zero exit (the ASAN
// abort) fails the test.
fp.off();
rst.stopSet(/* signal */ undefined, /* forRestart */ false, {skipValidation: false});

// Best-effort join. The parallel shell will see the connection close.
wedgedWriter();
