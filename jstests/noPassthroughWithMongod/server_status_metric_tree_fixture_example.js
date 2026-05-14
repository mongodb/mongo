/**
 * Example/demonstration test for jstests/libs/server_status_metric_tree.js.
 *
 * Companion fixture for SERVER-125804 (improve MetricTree + ServerStatus
 * testability). The fixture itself lives in jstests/libs/; this file is the
 * "show, don't tell" -- it walks through every helper against a real mongod
 * so reviewers can see the surface shape without context-switching to a
 * sharded/replset harness.
 *
 * Exercised helpers:
 *   - MetricTreeSnapshot.capture / .get / .has / .assertPathsPresent
 *   - getServerStatusMetric (one-shot path read)
 *   - assertMetricPath (one-shot read + predicate)
 *   - diffMetricSnapshots (counter delta over a workload)
 *   - assertMetricDeltaAtLeast (min-delta assertion, background-traffic safe)
 *
 *  @tags: [requires_fcv_63]
 */

import {
    MetricTreeSnapshot,
    assertMetricDeltaAtLeast,
    assertMetricPath,
    diffMetricSnapshots,
    getServerStatusMetric,
} from "jstests/libs/server_status_metric_tree.js";

const conn = db.getMongo();

// ---- One-shot reads ---------------------------------------------------------

// `network.numRequests` is a root-level (leading-dot) counter, present on every
// mongod build. The fixture's leading-"." convention matches MetricTree's
// "absolute path" semantics documented in server_status_metric.h.
assertMetricPath(conn, ".network.numRequests", (v) => typeof v === "number" && v >= 0);

// Paths without a leading dot are implicitly rooted under "metrics." -- the
// MetricTree default. Pick a well-known counter that exists on standalone mongod.
const cmdCount = getServerStatusMetric(conn, "commands.serverStatus.total");
assert.neq(cmdCount, undefined, "metrics.commands.serverStatus.total missing");

// ---- Snapshot + delta -------------------------------------------------------

const before = MetricTreeSnapshot.capture(conn);
assert(before.has(".network.numRequests"), "snapshot should expose .network paths");
assert.eq(before.has("definitely.not.a.real.metric.path"), false);

// Drive a small, deterministic workload: 5 explicit serverStatus calls. Each
// one is itself a request, and each one increments commands.serverStatus.total.
const workloadIterations = 5;
for (let i = 0; i < workloadIterations; i++) {
    assert.commandWorked(conn.adminCommand({serverStatus: 1}));
}

const after = MetricTreeSnapshot.capture(conn);

// Raw diff -- handy when a test wants to log the deltas before asserting.
const delta = diffMetricSnapshots(after, before, [
    ".network.numRequests",
    "commands.serverStatus.total",
]);
jsTest.log.info({"server_status_metric_tree_fixture_example deltas": delta});

// `commands.serverStatus.total` should have grown by at least `workloadIterations`
// (the 5 explicit calls). Background work may push it higher; the fixture's
// `assertMetricDeltaAtLeast` is the right shape for that.
assertMetricDeltaAtLeast(before, after, {
    "commands.serverStatus.total": workloadIterations,
});

// `.network.numRequests` counts every command including the serverStatus calls
// the fixture itself makes for capture(). It must have moved by at least the
// workload count too.
assertMetricDeltaAtLeast(before, after, {
    ".network.numRequests": workloadIterations,
});

// ---- assertPathsPresent batch read ----------------------------------------

// Lets a single test assert on many paths in one shot. The returned map is the
// `{path: value}` slice of the snapshot, useful for chained range checks.
const values = after.assertPathsPresent([
    ".network.numRequests",
    "commands.serverStatus.total",
]);
assert.gt(values[".network.numRequests"], 0);
assert.gt(values["commands.serverStatus.total"], 0);
