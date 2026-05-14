/**
 * SERVER-65418 repro: demonstrates that write-conflict retry loops sleep while
 * still holding global write tickets, blocking unrelated incoming writes for the
 * duration of the backoff.
 *
 * Setup
 *   - Single-node replset; global write tickets capped well below the conflict
 *     fanout so the backoff phase is forced to coexist with new arrivals.
 *   - Many concurrent updates target a single _id document (the canonical write-
 *     conflict storm shape called out in the ticket). Once attempt count > 3 the
 *     server enters logAndBackoff() sleeps while the operation continues to own
 *     its global write ticket and document/collection locks.
 *
 * Signal
 *   - A separate "victim" writer hits an unrelated namespace. Its observed
 *     latency under storm == time spent waiting on a ticket that the sleeping
 *     retriers refuse to release. With the proposed fix
 *     (saveLockStateAndUnlock around the backoff sleep) victim latency tracks
 *     normal queue behaviour rather than the storm's tail.
 *
 * @tags: [requires_replication, requires_persistence, multiversion_incompatible]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Keep the write ticket pool small so backoff-induced ticket starvation is
// observable without launching thousands of clients.
const kNumWriteTickets = 8;
const kStormWriters = 32;
const kStormOpsPerWriter = 40;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            executionControlConcurrentWriteTransactions: kNumWriteTickets,
            // Pin algorithm so the ticket cap is honoured rather than auto-tuned.
            executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            // Log every op so we can correlate ticket-wait time with retries.
            slowMS: 1,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB("admin");
const dbName = jsTestName();
const stormCollName = "storm";
const victimCollName = "victim";
const db = primary.getDB(dbName);

assert.commandWorked(db[stormCollName].insert({_id: "hot", n: 0}));
assert.commandWorked(db[victimCollName].insert({_id: "cold", n: 0}));

// Lower the slow-op threshold so ticket-wait time lands in serverStatus/log.
assert.commandWorked(db.setProfilingLevel(1, {slowms: 1}));

TestData.dbName = dbName;
TestData.stormCollName = stormCollName;
TestData.victimCollName = victimCollName;
TestData.stormOps = kStormOpsPerWriter;

// Spawn the storm: every shell hammers _id:"hot" in the same collection, which
// guarantees WriteConflictException on all but one writer per attempt. After
// numAttempts > 3 each retrier enters logAndBackoff() and sleeps WITH its
// global write ticket still acquired -- this is the resource-leak path the
// proposed fix targets.
const stormShells = [];
for (TestData.workerId = 0; TestData.workerId < kStormWriters; TestData.workerId++) {
    stormShells.push(
        startParallelShell(function () {
            const sdb = db.getSiblingDB(TestData.dbName);
            const c = sdb[TestData.stormCollName];
            for (let i = 0; i < TestData.stormOps; i++) {
                // Single-doc update on the same _id forces a contention point.
                assert.commandWorked(
                    c.update({_id: "hot"}, {$inc: {n: 1}}, {writeConcern: {w: 1}}),
                );
            }
        }, primary.port),
    );
}

// Sample victim latency while the storm runs. The victim touches an unrelated
// collection and an unrelated _id, so under correct ticket discipline its
// per-op latency should remain bounded by ticket-acquisition time + WT commit.
// With the resource-leak bug present, the victim is stalled behind sleeping
// retriers and its p95 latency balloons.
const kVictimSamples = 50;
const victimLatenciesMs = [];
for (let i = 0; i < kVictimSamples; i++) {
    const t0 = Date.now();
    assert.commandWorked(
        db[victimCollName].update(
            {_id: "cold"},
            {$inc: {n: 1}},
            {writeConcern: {w: 1}},
        ),
    );
    victimLatenciesMs.push(Date.now() - t0);
}

stormShells.forEach((s) => s());

victimLatenciesMs.sort((a, b) => a - b);
const p50 = victimLatenciesMs[Math.floor(kVictimSamples * 0.5)];
const p95 = victimLatenciesMs[Math.floor(kVictimSamples * 0.95)];
const pMax = victimLatenciesMs[kVictimSamples - 1];

jsTestLog(
    "SERVER-65418 victim latency (unrelated _id under storm): " +
        `p50=${p50}ms p95=${p95}ms max=${pMax}ms ` +
        `tickets=${kNumWriteTickets} stormWriters=${kStormWriters}`,
);

// serverStatus counters quantify the leak: timeAcquiringMicros must climb in
// step with totalTimeQueuedMicros when retriers sleep on a held ticket.
const ss = assert.commandWorked(adminDb.runCommand({serverStatus: 1}));
const ticketStats = ss.wiredTiger && ss.wiredTiger.concurrentTransactions
    ? ss.wiredTiger.concurrentTransactions.write
    : (ss.queues && ss.queues.execution && ss.queues.execution.write);
jsTestLog("SERVER-65418 write-ticket counters: " + tojson(ticketStats));

// Diagnostic-only thresholds. The test is wired as a repro/observation harness
// rather than a regression gate -- the fix changes p95 by ~10x in local runs,
// but absolute numbers are host-sensitive.  Flip these to assert.lt once the
// fix lands and the steady-state numbers stabilise across the perf fleet.
const kDiagnosticP95Ceiling = 250; // ms; pre-fix observation: p95 >> 1000ms.
if (p95 > kDiagnosticP95Ceiling) {
    jsTestLog(
        "SERVER-65418 OBSERVED LEAK: victim p95 " + p95 +
        "ms exceeds " + kDiagnosticP95Ceiling +
        "ms ceiling -- consistent with retriers holding tickets across backoff sleeps.",
    );
}

rst.stopSet();
