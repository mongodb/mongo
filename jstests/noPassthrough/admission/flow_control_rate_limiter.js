/**
 * Tests that flow control correctly routes writes through the rate limiter when
 * flowControlUseRateLimiter is enabled, and falls back to the legacy ticketholder when disabled.
 *
 * Verifies:
 * - serverStatus exposes flowControl.rateLimiter stats
 * - successfulAdmissions increments when the rate limiter path is active
 * - toggling flowControlUseRateLimiter at runtime switches enforcement mechanism
 * - the rate limiter actually throttles when FC is engaged (lagged)
 *
 * @tags: [
 *   requires_flow_control,
 *   requires_majority_read_concern,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartReplicationOnSecondaries, stopReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";

const testName = jsTestName();

function getFlowControlStats(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1})).flowControl;
}

function doWrites(primary, count) {
    const db = primary.getDB(testName);
    for (let i = 0; i < count; i++) {
        assert.commandWorked(db.getCollection("data").insert({x: i, ts: new Date()}));
    }
}

// --- Phase 1: Rate limiter stats are present and increment with flag ON ---

const replSet = new ReplSetTest({name: testName, nodes: 3});
replSet.startSet({
    setParameter: {
        enableFlowControl: true,
        flowControlUseRateLimiter: true,
        flowControlSamplePeriod: 1,
        flowControlTargetLagSeconds: 1,
        flowControlThresholdLagPercentage: 1,
        writePeriodicNoops: true,
        periodicNoopIntervalSecs: 2,
    },
});
replSet.initiate();

const primary = replSet.getPrimary();

{
    const stats = getFlowControlStats(primary);
    assert(
        stats.hasOwnProperty("rateLimiter"),
        "serverStatus.flowControl should contain rateLimiter subdocument: " + tojson(stats),
    );
    assert(
        stats.rateLimiter.hasOwnProperty("successfulAdmissions"),
        "rateLimiter should have successfulAdmissions: " + tojson(stats.rateLimiter),
    );
}

const admissionsBefore = getFlowControlStats(primary).rateLimiter.successfulAdmissions;

doWrites(primary, 50);

const admissionsAfterWrites = getFlowControlStats(primary).rateLimiter.successfulAdmissions;
assert.gt(
    admissionsAfterWrites,
    admissionsBefore,
    "successfulAdmissions should increase after writes with flowControlUseRateLimiter=true",
);

// --- Phase 2: Toggle flag OFF — admissions should stop growing ---

assert.commandWorked(primary.adminCommand({setParameter: 1, flowControlUseRateLimiter: false}));

const admissionsBeforeToggle = getFlowControlStats(primary).rateLimiter.successfulAdmissions;

doWrites(primary, 50);

const admissionsAfterToggle = getFlowControlStats(primary).rateLimiter.successfulAdmissions;
assert.eq(
    admissionsAfterToggle,
    admissionsBeforeToggle,
    "successfulAdmissions should not increase with flowControlUseRateLimiter=false",
);

// --- Phase 3: Toggle flag back ON — admissions resume ---

assert.commandWorked(primary.adminCommand({setParameter: 1, flowControlUseRateLimiter: true}));

const admissionsBeforeResume = getFlowControlStats(primary).rateLimiter.successfulAdmissions;

doWrites(primary, 50);

const admissionsAfterResume = getFlowControlStats(primary).rateLimiter.successfulAdmissions;
assert.gt(
    admissionsAfterResume,
    admissionsBeforeResume,
    "successfulAdmissions should resume increasing after re-enabling rate limiter",
);

// --- Phase 4: Rate limiter throttles when FC is engaged (lagged) ---

stopReplicationOnSecondaries(replSet);

doWrites(primary, 20);

assert.soon(
    () => {
        const stats = getFlowControlStats(primary);
        return stats.isLagged === true;
    },
    "Flow control should detect lag after stopping replication",
    30000,
    1000,
);

const statsWhileLagged = getFlowControlStats(primary);
assert.lt(
    statsWhileLagged.targetRateLimit,
    1000 * 1000 * 1000,
    "targetRateLimit should be reduced below max when lagged: " + tojson(statsWhileLagged),
);

restartReplicationOnSecondaries(replSet);

replSet.stopSet();
