/**
 * Validates that time spent waiting for admission is measured across the entire life of an
 * operation, at every gate that holds the operation back.
 *
 * A single insert is driven through all four admission gates that construct a
 * WaitingForAdmissionGuard, with each gate made to hold it back:
 *
 *   - ingress_request: the ingress request rate limiter is armed with a one-token bucket which a
 *     preceding request drains, so the insert naps until a token is issued;
 *   - ingress: the ingress admission ticket pool is emptied, then refilled once the insert is
 *     observed waiting there;
 *   - execution: execution control is left without write tickets, likewise restored once the insert
 *     is observed waiting there;
 *   - writeThrottle: the write throttler is armed with a bucket that holds fewer tokens than the
 *     insert needs for its documents, so it naps partway through writing them.
 *
 * The sleepInWaitingForAdmissionGuard failpoint injects a fixed wait inside every guard, giving
 * each gate a wait large enough to survive the millisecond rounding of the reported durations. The
 * failpoint cannot manufacture queueing on its own: a gate constructs a guard only once it has
 * decided to make the operation wait, which is why the test also has to create real contention at
 * each of them.
 *
 * The test then asserts the waits are:
 *   - attributed to the queue of the gate that imposed them, in the per-gate "queues" breakdown of
 *     both the slow query log and the profiler,
 *   - counted towards the duration reported by both the slow query log and the profiler,
 *   - reported as blocked rather than working time,
 *   - carried into the cumulative queueing time each gate reports in serverStatus, so the
 *     process-wide totals are denominated in the same measured waits the per-operation surfaces
 *     report, and
 *   - split across serverStatus's opLatencies and opWorkingTime, the process-wide counters FTDC
 *     scrapes and the Atlas UI charts.
 *
 * The gates are opened and closed process-wide and the failpoint applies to every operation, so
 * this test runs its own mongod rather than sharing one.
 *
 * @tags: [requires_fcv_90]
 */

import {AdmissionQueue, waitForOperationToEnterQueue} from "jstests/libs/admission/queues.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {
    kRateLimiterExemptAppName,
    makeExemptConn,
    setupAuth,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";
import {getParameters, setParameters} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const kInjectedWaitMillis = 50;
const kMinMeasuredWaitMillis = kInjectedWaitMillis - 1;
const kIrrlRatePerSec = 2;
const kIrrlBurstCapacitySecs = 0.5;
const kIrrlMaxQueueDepth = 100;
const kWriteThrottleRatePerSec = 40;
const kWriteThrottleBurstCapacitySecs = 0.5;
const kInsertBatchSize = 8;
const kInsertedDocuments = 40;

const kComment = jsTestName();
const kCollName = "queueing";

let conn;
let exemptConn;
let adminDB;
let testDB;
let originalTicketParams;
let sleepFailPoint;

/**
 * Sums the queueing time reported for every gate in a "queues" breakdown, truncated to whole
 * milliseconds.
 */
function sumQueuedMillis(queues) {
    const micros = Object.values(queues).reduce(
        (total, gate) => total + (gate.totalTimeQueuedMicros ?? 0),
        0,
    );
    return Math.floor(micros / 1000);
}

function assertEveryGateReportsItsWait(queues, surface, context) {
    for (const gate of Object.values(AdmissionQueue)) {
        const queue = queues[gate];
        assert(queue !== undefined, `no ${gate} queue reported in the ${surface}`, context);
        assert.gte(
            queue.admissions,
            1,
            `the ${surface} should show the insert admitted by the ${gate} gate`,
            context,
        );
        assert.gte(
            queue.totalTimeQueuedMicros,
            kMinMeasuredWaitMillis * 1000,
            `the ${surface} should report the wait at the ${gate} gate as its queueing time`,
            context,
        );
    }
}

/**
 * Returns the cumulative queueing time each gate reports process-wide in serverStatus, keyed by the
 * queue name the per-operation surfaces use for that gate.
 *
 * Execution control keeps a pool per operation type, and the per-operation surfaces report one
 * execution queue covering both, so the pools are summed back together here. Only the normal
 * priority pools are read: the test leaves deprioritization off, so the insert is admitted there.
 */
function getCumulativeQueuedMicros() {
    const status = assert.commandWorked(adminDB.runCommand({serverStatus: 1}));
    const execution = status.queues.execution;
    return {
        [AdmissionQueue.IngressRequest]:
            status.network.ingressRequestRateLimiter.totalTimeQueuedMicros,
        [AdmissionQueue.Ingress]: status.queues.ingress.normalPriority.totalTimeQueuedMicros,
        [AdmissionQueue.Execution]:
            execution.read.normalPriority.totalTimeQueuedMicros +
            execution.write.normalPriority.totalTimeQueuedMicros,
        [AdmissionQueue.WriteThrottle]: status.queues.writeThrottler.totalTimeQueuedMicros,
    };
}

/**
 * Returns the cumulative write-operation totals from the two serverStatus sections that end up in
 * FTDC and the Atlas UI: `opLatencies` accumulates total elapsed time and `opWorkingTime`
 * accumulates the same operations' time excluding blocked intervals. Latencies are microseconds.
 */
function getCumulativeWriteLatencies() {
    const status = assert.commandWorked(adminDB.runCommand({serverStatus: 1}));
    return {
        totalTimeMicros: status.opLatencies.writes.latency,
        workingTimeMicros: status.opWorkingTime.writes.latency,
        ops: status.opLatencies.writes.ops,
    };
}

/** Returns the attributes of the slow query log line for the operation tagged `kComment`. */
function getSlowQueryLogAttrs() {
    let attrs;
    assert.soon(
        () => {
            const globalLog = assert.commandWorked(adminDB.runCommand({getLog: "global"}));
            const line = findMatchingLogLine(globalLog.log, {id: 51803, comment: kComment});
            if (line === null) {
                return false;
            }
            attrs = JSON.parse(line).attr;
            return true;
        },
        "expected a slow query log line for the insert tagged '" + kComment + "'",
    );
    return attrs;
}

/** Returns the system.profile entry for the operation tagged `kComment`. */
function getProfileEntry() {
    let entry;
    assert.soon(
        () => {
            entry = testDB.system.profile.findOne({"command.comment": kComment});
            return entry !== null;
        },
        "expected a system.profile entry for the insert tagged '" + kComment + "'",
    );
    return entry;
}

/**
 * Runs the measured insert on its own authenticated, non-exempt connection, and returns its raw
 * command result. The insert closes the ingress and execution gates itself, and arms the rate limiter,
 * rather than having the control connection do it, since closing the gates from outside would strand
 * this connection before it ever issued the insert.
 */
async function runMeasuredInsert(
    host,
    dbName,
    collName,
    comment,
    documentCount,
    irrlRatePerSec,
    irrlBurstCapacitySecs,
    irrlMaxQueueDepth,
) {
    const {makeAuthConn} = await import(
        "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js"
    );
    const {setParameters} = await import("jstests/noPassthrough/libs/server_parameter_helpers.js");
    // Authenticating is what subjects this connection to the rate limiter, on top of granting it
    // the privileges to close the gates below.
    const insertConn = makeAuthConn(host);
    // setParameter is not itself subject to ingress admission control, so this connection can keep
    // configuring the server after emptying the pool.
    assert.commandWorked(
        setParameters(insertConn, {
            ingressAdmissionControllerTicketPoolSize: 0,
            executionControlConcurrentWriteTransactions: 0,
        }),
    );
    assert.commandWorked(
        setParameters(insertConn, {
            ingressRequestAdmissionRatePerSec: irrlRatePerSec,
            ingressRequestAdmissionBurstCapacitySecs: irrlBurstCapacitySecs,
            // Queueing is disabled by default, which makes the limiter reject requests outright
            // instead of holding them for a token.
            ingressRequestAdmissionMaxQueueDepth: irrlMaxQueueDepth,
            ingressRequestRateLimiterEnabled: 1,
        }),
    );

    // Drain the single token the bucket is armed with, so the insert that follows has to wait for
    // the limiter to issue a new one.
    assert.commandWorked(insertConn.adminCommand({ping: 1}));

    return insertConn.getDB(dbName).runCommand({
        insert: collName,
        documents: Array.from({length: documentCount}, (_, i) => ({x: i})),
        comment: comment,
    });
}

describe("admission queueing time accounting", function () {
    before(function () {
        conn = MongoRunner.runMongod({
            auth: "",
            setParameter: {
                // Arm the write throttler so writes pass through its admission gate, with a bucket
                // too small to cover the measured insert in full.
                writeThrottlerEnabled: true,
                writeThrottlerTargetRatePerSec: kWriteThrottleRatePerSec,
                writeThrottlerBurstCapacitySecs: kWriteThrottleBurstCapacitySecs,
                internalInsertMaxBatchSize: kInsertBatchSize,
                // Hold execution control's ticket count where this test sets it: throughput probing
                // would otherwise adjust it back up on its own.
                executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
                // Keep the insert in the normal-priority pool, which is the one the test empties.
                executionControlDeprioritizationGate: false,
                // Exempt the control connection from the rate limiter, so that arming the limiter
                // does not also throttle the commands that drive the test.
                ingressRequestRateLimiterApplicationExemptions: {
                    appNames: [kRateLimiterExemptAppName],
                },
            },
        });

        exemptConn = makeExemptConn(conn.host, {authenticate: false});
        setupAuth(conn, exemptConn);
        adminDB = exemptConn.getDB("admin");
        testDB = exemptConn.getDB(jsTestName());
        originalTicketParams = getParameters(exemptConn, [
            "ingressAdmissionControllerTicketPoolSize",
            "executionControlConcurrentWriteTransactions",
        ]);

        // $currentOp reports the per-gate queueing breakdown only while an operation is still in
        // flight, so the two surfaces that have it for a finished operation are the slow query log
        // and the profiler: level 2 captures every operation in system.profile, and slowms 0
        // guarantees a log line as well. Note that the slow query threshold is compared against
        // working time, which by design excludes the waits this test induces.
        testDB.setProfilingLevel(2, {slowms: 0});

        // Create the collection up front so the measured insert is not also an implicit create.
        assert.commandWorked(testDB.createCollection(kCollName));
    });

    after(function () {
        if (!conn) {
            return;
        }
        // Leave the gates open and the failpoint disarmed so shutdown is not held back. The
        // failpoint is normally disarmed as soon as the insert completes, so this only has anything
        // to do when the test failed while it was armed.
        sleepFailPoint?.off();
        assert.commandWorked(
            setParameters(exemptConn, {
                ...originalTicketParams,
                ingressRequestRateLimiterEnabled: 0,
            }),
        );
        MongoRunner.stopMongod(conn);
    });

    describe("with every gate holding the operation back", function () {
        let logAttrs;
        let profileEntry;
        let cumulativeQueuedMicrosBefore;
        let cumulativeQueuedMicrosAfter;
        let writeLatenciesBefore;
        let writeLatenciesAfter;

        before(function () {
            sleepFailPoint = configureFailPoint(exemptConn, "sleepInWaitingForAdmissionGuard", {
                ms: kInjectedWaitMillis,
            });

            cumulativeQueuedMicrosBefore = getCumulativeQueuedMicros();
            writeLatenciesBefore = getCumulativeWriteLatencies();

            const insertThread = new Thread(
                runMeasuredInsert,
                conn.host,
                jsTestName(),
                kCollName,
                kComment,
                kInsertedDocuments,
                kIrrlRatePerSec,
                kIrrlBurstCapacitySecs,
                kIrrlMaxQueueDepth,
            );
            insertThread.start();

            // The insert waits out its rate limiter nap on its own, then blocks on the empty
            // ingress pool. Refill the pool once it is there, which lets it reach the write
            // throttler and then block again on the missing write ticket.
            waitForOperationToEnterQueue(adminDB, kComment, AdmissionQueue.Ingress);
            assert.commandWorked(
                setParameters(exemptConn, {
                    ingressAdmissionControllerTicketPoolSize:
                        originalTicketParams.ingressAdmissionControllerTicketPoolSize,
                }),
            );

            waitForOperationToEnterQueue(adminDB, kComment, AdmissionQueue.Execution);
            assert.commandWorked(
                setParameters(exemptConn, {
                    executionControlConcurrentWriteTransactions:
                        originalTicketParams.executionControlConcurrentWriteTransactions,
                }),
            );

            insertThread.join();
            assert.commandWorked(insertThread.returnData(), "the measured insert should have run");

            sleepFailPoint.off();
            assert.commandWorked(setParameters(exemptConn, {ingressRequestRateLimiterEnabled: 0}));

            cumulativeQueuedMicrosAfter = getCumulativeQueuedMicros();

            logAttrs = getSlowQueryLogAttrs();
            profileEntry = getProfileEntry();
            writeLatenciesAfter = getCumulativeWriteLatencies();
        });

        it("attributes a wait to every gate the operation passed through", function () {
            assertEveryGateReportsItsWait(logAttrs.queues, "slow query log", {logAttrs});
            assertEveryGateReportsItsWait(profileEntry.queues, "profiler entry", {profileEntry});
        });

        it("reports the same per-gate queueing in the slow query log and the profiler", function () {
            for (const gate of Object.values(AdmissionQueue)) {
                assert.eq(
                    logAttrs.queues[gate].totalTimeQueuedMicros,
                    profileEntry.queues[gate].totalTimeQueuedMicros,
                    `the two surfaces should report the same wait at the ${gate} gate`,
                    {logAttrs, profileEntry},
                );
            }
        });

        it("carries the waits into the cumulative totals serverStatus reports", function () {
            for (const gate of Object.values(AdmissionQueue)) {
                const operationMicros = logAttrs.queues[gate].totalTimeQueuedMicros;
                const cumulativeMicros =
                    cumulativeQueuedMicrosAfter[gate] - cumulativeQueuedMicrosBefore[gate];
                // The cumulative counters are process-wide, so anything else that queued at this
                // gate while the insert was in flight is in here too, which makes the operation's
                // own wait a lower bound rather than the whole of it. The bound still has teeth:
                // most of each wait here is injected by the failpoint, which only the guard around
                // the wait can see, so a gate reporting anything other than measured time falls
                // well short of it.
                assert.gte(
                    cumulativeMicros,
                    operationMicros,
                    `serverStatus should count the insert's wait at the ${gate} gate towards its ` +
                        "cumulative queueing time",
                    {logAttrs, cumulativeQueuedMicrosBefore, cumulativeQueuedMicrosAfter},
                );
            }
        });

        it("counts the waits at every gate towards the duration of the operation", function () {
            // The gates are all reached before or during command execution, so the waits are only
            // visible here if admission time is attributed to the operation from the moment it
            // enters the server.
            const queuedMillis = sumQueuedMillis(logAttrs.queues);
            assert.gte(
                queuedMillis,
                Object.values(AdmissionQueue).length * kMinMeasuredWaitMillis,
                "every gate should have contributed its wait",
                {logAttrs},
            );
            assert.gte(
                logAttrs.durationMillis,
                queuedMillis,
                "the operation duration should cover the waits at every gate",
                {logAttrs},
            );
            // The profiler reports elapsed time excluding intervals where the operation timer was
            // paused, which is at most the total elapsed time the log reports. Requiring it to
            // cover the waits as well pins down that admission does not pause the timer and thereby
            // discount them.
            assert.gte(
                profileEntry.millis,
                queuedMillis,
                "the profiled execution time should cover the waits at every gate",
                {profileEntry},
            );
            assert.gte(
                logAttrs.durationMillis,
                profileEntry.millis,
                "total elapsed time cannot be less than elapsed time excluding pauses",
                {logAttrs, profileEntry},
            );
        });

        it("reports the waits as blocked rather than working time", function () {
            const queuedMillis = sumQueuedMillis(logAttrs.queues);
            // Blocked time also covers lock waits and prepare conflicts, so queueing time is a
            // lower bound on it rather than the whole of it.
            assert.gte(
                logAttrs.durationMillis - logAttrs.workingMillis,
                queuedMillis,
                "queueing time should be excluded from working time",
                {logAttrs},
            );
        });

        it("separates the waits across serverStatus opLatencies and opWorkingTime", function () {
            // These are the counters FTDC scrapes and the Atlas UI charts, so the split has to hold
            // here and not only in the per-operation surfaces above.
            const queuedMicros = sumQueuedMillis(logAttrs.queues) * 1000;
            const context = {logAttrs, writeLatenciesBefore, writeLatenciesAfter};

            assert.gte(
                writeLatenciesAfter.ops - writeLatenciesBefore.ops,
                1,
                "serverStatus should have counted the insert as a write",
                context,
            );

            // The counters are process-wide and the insert's preamble writes too, so every delta
            // here covers more than the measured insert alone. That only ever adds time, which
            // keeps the insert's own waits a valid lower bound.
            const totalTimeMicros =
                writeLatenciesAfter.totalTimeMicros - writeLatenciesBefore.totalTimeMicros;
            assert.gte(
                totalTimeMicros,
                queuedMicros,
                "opLatencies should count the waits towards total write latency",
                context,
            );

            const workingTimeMicros =
                writeLatenciesAfter.workingTimeMicros - writeLatenciesBefore.workingTimeMicros;
            assert.gte(
                totalTimeMicros - workingTimeMicros,
                queuedMicros,
                "opWorkingTime should exclude the waits opLatencies counts",
                context,
            );
        });
    });
});
