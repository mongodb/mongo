/**
 * SERVER-115688 — PALI failover telemetry audit gate.
 *
 * This test is the executable form of the audit at
 *   src/mongo/db/disagg/pali/PALI-FAILOVER-TELEMETRY-SERVER-115688.md
 *
 * It stands up a 3-node replica set, forces a step-up from node 1, and asserts that
 * `replSetGetStatus` / `serverStatus` expose the six telemetry surfaces the audit identifies as
 * load-bearing for diagnosing PALI failovers:
 *
 *   1. electionMetrics.preStepUpWaitMillis              (transition standby-exit)
 *   2. electionMetrics.catchupDurationMillis            (transition catchup)
 *   3. electionMetrics.standbyToPrimaryMillis           (transition standby-exit -> become-primary)
 *   4. electionMetrics.postStepUpFirstWriteMillis       (transition become-primary)
 *   5. paliZoneAffinity                                 (zone-binding visibility)
 *   6. paliPageServerRequests                           (per-endpoint request counters)
 *
 * None of these surfaces exist in `master` today. Every assertion below is expected to fail until
 * the metrics in the audit doc are implemented; the test documents the deliverable.
 *
 * Tag: this test is intentionally NOT added to any passing-suite tag set. It is a regression gate
 * that flips green once SERVER-115688 implementation work lands.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_80,
 *   featureFlagPaliFailoverTelemetry,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = jsTestName();
const rst = new ReplSetTest({name: testName, nodes: 3});
rst.startSet();
rst.initiate();
rst.awaitReplication();

const originalPrimary = rst.getPrimary();
const secondaries = rst.getSecondaries();
const stepUpTarget = secondaries[0];

jsTestLog(`Forcing step-up on ${stepUpTarget.host}`);

// Drive a user-visible write on the original primary so post-step-up first-write latency is
// non-trivial to observe.
const collName = "pali_telemetry_gates";
assert.commandWorked(originalPrimary.getDB("test")[collName].insert({_id: 0, seedPriorStepUp: true}));
rst.awaitReplication();

// Force the step-up. stepUpNoAwaitReplication is the canonical helper for telemetry tests; we want
// the transitions to fire without ReplSetTest waiting on a quiesced state we are about to probe.
rst.stepUp(stepUpTarget);
rst.awaitNodesAgreeOnPrimary();
const newPrimary = rst.getPrimary();
assert.eq(newPrimary.host, stepUpTarget.host, "step-up target did not become primary");

// Issue the first user-visible write under the new primary so postStepUpFirstWriteMillis has a
// defined value to inspect.
assert.commandWorked(newPrimary.getDB("test")[collName].insert({_id: 1, postStepUp: true}));

const status = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
const serverStatus = assert.commandWorked(newPrimary.adminCommand({serverStatus: 1}));
jsTestLog(`replSetGetStatus on new primary: ${tojson(status)}`);

const em = status.electionMetrics || {};

/**
 * Helper: assert a named numeric field is present and >= 0. Failure message names the audit
 * section so a reviewer of a red test run can locate the matching paragraph in the .md.
 */
function assertNumericMetric(obj, path, auditSection) {
    const value = path.split(".").reduce((acc, key) => (acc == null ? acc : acc[key]), obj);
    assert(value !== undefined && value !== null,
           `[SERVER-115688 audit ${auditSection}] missing metric '${path}': ${tojson(obj)}`);
    assert(typeof value === "number" && value >= 0,
           `[SERVER-115688 audit ${auditSection}] metric '${path}' is not a non-negative number: ${tojson(value)}`);
}

// Audit gap #1: preStepUpWaitMillis — RSTL acquisition wait.
assertNumericMetric(em, "preStepUpWaitMillis", "Gap 1");

// Audit gap #2: catchupDurationMillis — wall time of catchup phase.
assertNumericMetric(em, "catchupDurationMillis", "Gap 2");

// Audit gap #3: standbyToPrimaryMillis — RSTL-acquired to transition-complete.
assertNumericMetric(em, "standbyToPrimaryMillis", "Gap 3");

// Audit gap #4: postStepUpFirstWriteMillis — transition-complete to first user write applied.
assertNumericMetric(em, "postStepUpFirstWriteMillis", "Gap 4");

// Audit gap #5: paliZoneAffinity block — current zone and bound endpoint sets.
const zone = serverStatus.paliZoneAffinity;
assert(zone,
       "[SERVER-115688 audit Gap 5] serverStatus.paliZoneAffinity missing. Expected " +
           "{currentZone, pageServerEndpoints[], logServerEndpoints[], outOfZoneRequests24h}");
assert(typeof zone.currentZone === "string" && zone.currentZone.length > 0,
       `[SERVER-115688 audit Gap 5] paliZoneAffinity.currentZone is not a non-empty string: ${tojson(zone)}`);
assert(Array.isArray(zone.pageServerEndpoints),
       `[SERVER-115688 audit Gap 5] paliZoneAffinity.pageServerEndpoints not an array: ${tojson(zone)}`);
assert(Array.isArray(zone.logServerEndpoints),
       `[SERVER-115688 audit Gap 5] paliZoneAffinity.logServerEndpoints not an array: ${tojson(zone)}`);
assertNumericMetric(zone, "outOfZoneRequests24h", "Gap 5");

// Audit gap #6: per-page-server request counters keyed by endpoint.
const pageReqs = serverStatus.paliPageServerRequests;
assert(pageReqs && typeof pageReqs === "object",
       "[SERVER-115688 audit Gap 6] serverStatus.paliPageServerRequests missing. Expected an " +
           "object keyed by endpoint with {inFlight, errors, p99LatencyMillis} per entry.");
const endpoints = Object.keys(pageReqs);
assert(endpoints.length > 0,
       `[SERVER-115688 audit Gap 6] paliPageServerRequests has no endpoints: ${tojson(pageReqs)}`);
for (const ep of endpoints) {
    const counters = pageReqs[ep];
    assertNumericMetric(counters, "inFlight", `Gap 6 (${ep})`);
    assertNumericMetric(counters, "errors", `Gap 6 (${ep})`);
    assertNumericMetric(counters, "p99LatencyMillis", `Gap 6 (${ep})`);
}

jsTestLog("All PALI failover telemetry gates passed. SERVER-115688 audit deliverable is complete.");
rst.stopSet();
