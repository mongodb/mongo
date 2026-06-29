/**
 * Tests that TTL deletions and index builds report their execution ticket statistics — time queued
 * for tickets, time processing while holding tickets, and admission counts — in serverStatus and in
 * their per-operation log lines.
 *
 * Each case forces the task to wait for a ticket by zeroing the low-priority ticket pools, waiting
 * until it is parked in the queue, then granting a single ticket. This makes the queued-time
 * counters advance deterministically rather than relying on incidental contention.
 *
 * @tags: [
 *   # The test deploys replica sets with execution control concurrency adjustment configured by
 *   # each test case, which should not be overwritten and expect to have 'throughputProbing' as
 *   # default.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {insertTestDocuments} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

const kNumDocs = 1000;

// Attribute predicates shared by all per-operation ticket-stats log lines. The check is loose for
// the queued fields because a log line attributes ticket cost to a single unit of work, which does
// not necessarily cover the operation's whole execution — so the queue wait may land elsewhere and
// read 0 on a given line. Admissions and processing time, by contrast, are always incurred.
const kTicketStatsLogAttrs = {
    timeQueuedForTicketsMicros: (v) => v >= 0,
    timeProcessingWithTicketsMicros: (v) => v > 0,
    ticketAdmissions: (v) => v > 0,
    lowPriorityTicketAdmissions: (v) => v > 0,
    ticketQueueEntries: (v) => v >= 0,
};

const kTicketStatsFields = {
    admissions: "ticketAdmissions",
    lowPriority: "lowPriorityTicketAdmissions",
    processing: "timeProcessingWithTicketsMicros",
    queued: "timeQueuedForTicketsMicros",
    queuedGauge: "queuedForTickets",
};

function setLowPriorityTickets(conn, numTickets) {
    assert.commandWorked(
        conn.adminCommand({
            setParameter: 1,
            executionControlConcurrentReadLowPriorityTransactions: numTickets,
            executionControlConcurrentWriteLowPriorityTransactions: numTickets,
        }),
    );
}

// Releases a background task that has been parked in the ticket queue by 'setLowPriorityTickets(0)'.
// Waits until the task's currently-queued gauge confirms it is actually waiting for a ticket — so
// it accrues real queued time — then grants a single low-priority ticket to let it proceed.
function awaitQueuedThenRelease(conn, getStats, queuedGaugeField, taskName) {
    assert.soon(
        () => (getStats()[queuedGaugeField] ?? 0) > 0,
        () => `${taskName} did not queue for a low-priority ticket`,
    );
    setLowPriorityTickets(conn, 1);
}

// Waits for the task's ticket stats counters to increase past 'beforeStats'. The counters are
// updated continuously while the task runs (at every ticket acquisition/release), but may
// slightly trail the visible completion of its work, hence the polling.
function awaitTicketStatsIncreased(getStats, beforeStats, fields) {
    // Counters may not be rendered in serverStatus until they are first incremented, so treat
    // missing 'before' fields as zero.
    const before = (name) => beforeStats[name] ?? 0;

    let afterStats;
    assert.soon(
        () => {
            afterStats = getStats();
            for (const [key, name] of Object.entries(fields)) {
                // The currently-queued gauge may be absent until the task first queues.
                if (key === "queuedGauge") {
                    continue;
                }
                assert(afterStats.hasOwnProperty(name), `missing field ${name}`, {afterStats});
            }
            return (
                // The task acquired tickets and spent time processing while holding them.
                afterStats[fields.admissions] > before(fields.admissions) &&
                afterStats[fields.processing] > before(fields.processing) &&
                afterStats[fields.lowPriority] > before(fields.lowPriority) &&
                afterStats[fields.queued] > before(fields.queued)
            );
        },
        () => "ticket stats did not increase: " + tojson({beforeStats, afterStats}),
    );

    // The currently-queued gauge must never go negative, and returns to 0 once the task drains
    // the queue and completes.
    assert.gte(afterStats[fields.queuedGauge] ?? 0, 0, "currently-queued gauge went negative", {
        afterStats,
    });
}

describe("Background task ticket admission statistics", function () {
    describe("TTL deletions and index builds", function () {
        let replTest, primary, db, coll;

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        ttlMonitorSleepSecs: 1,
                        ttlMonitorEnabled: false,
                        ttlMonitorBackgroundOperation: true,
                        executionControlDeprioritizationGate: true,
                        executionControlHeuristicDeprioritization: false,
                        internalQueryExecYieldIterations: 1,
                    },
                    // Log every slow operation so the per-operation log lines can be asserted on.
                    slowms: 0,
                },
            });
            replTest.startSet();
            replTest.initiate();
            primary = replTest.getPrimary();
            db = primary.getDB(jsTestName());
            coll = db.coll;
        });

        after(function () {
            replTest.stopSet();
        });

        it("reports TTL ticket stats in serverStatus and the TTL deletion log line", function () {
            assert.commandWorked(coll.createIndex({expireAt: 1}, {expireAfterSeconds: 0}));

            const pastDate = new Date(Date.now() - 5000);
            insertTestDocuments(coll, kNumDocs, {
                payloadSize: 1024,
                docGenerator: (id, payload) => ({_id: id, expireAt: pastDate, payload: payload}),
            });

            const getStats = () => primary.getDB("admin").serverStatus().metrics.ttl;
            const before = getStats();

            // Zero the low-priority ticket pools so the TTL deletion parks in the ticket queue, then
            // enable the monitor so a pass begins and stalls on ticket admission.
            setLowPriorityTickets(primary, 0);
            assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));
            awaitQueuedThenRelease(
                primary,
                getStats,
                kTicketStatsFields.queuedGauge,
                "TTL deletion",
            );

            assert.soon(
                () => coll.countDocuments({}) === 0,
                "TTL monitor did not delete documents",
            );

            awaitTicketStatsIncreased(getStats, before, kTicketStatsFields);
            assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

            // "Deleted expired documents using index" must carry the ticket stats attributes.
            assert(
                checkLog.checkContainsOnceJson(primary, 5479200, kTicketStatsLogAttrs),
                "TTL deletion log line missing required ticket stats attributes",
            );
        });

        it("reports index build ticket stats in serverStatus and the build completion log line", function () {
            insertTestDocuments(coll, kNumDocs, {payloadSize: 1024});

            const getStats = () => primary.getDB("admin").serverStatus().indexBuilds;
            const before = getStats();

            // Zero the low-priority ticket pools so the build parks in the ticket queue, then start
            // the build in a parallel shell (createIndex blocks until the build completes).
            setLowPriorityTickets(primary, 0);
            const awaitIndexBuild = startParallelShell(
                funWithArgs(function (dbName) {
                    assert.commandWorked(db.getSiblingDB(dbName).coll.createIndex({payload: 1}));
                }, db.getName()),
                primary.port,
            );
            awaitQueuedThenRelease(
                primary,
                getStats,
                kTicketStatsFields.queuedGauge,
                "index build",
            );
            awaitIndexBuild();

            awaitTicketStatsIncreased(getStats, before, kTicketStatsFields);

            // "Index build: completed successfully" must carry the ticket stats attributes.
            assert(
                checkLog.checkContainsOnceJson(primary, 20663, kTicketStatsLogAttrs),
                "index build completion log line missing required ticket stats attributes",
            );
        });
    });
});
