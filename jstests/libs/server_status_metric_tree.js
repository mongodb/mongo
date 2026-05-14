/**
 * Test fixture for asserting on the serverStatus MetricTree.
 *
 * Motivation (SERVER-125804):
 *   The C++ side of MetricTree+ServerStatus reads metrics through a static
 *   `globalMetricTreeSet()` (see src/mongo/db/commands/server_status/server_status_command.cpp
 *   `treeSet = globalMetricTreeSet()`). Even with the test-only `setTreeSet()` and
 *   `removeForTests()` hooks on the C++ side, JS integration tests still rely on
 *   ad-hoc walks like `ss.metrics.repl.network.oplogGetMoresProcessed.num` --
 *   repetitive, easy to mis-spell, and silently degrade to `undefined` when a
 *   metric path is renamed or guarded by a feature flag.
 *
 *   This module gives jstests a small, typed surface for reading the MetricTree
 *   over the wire so the brittle bits live in one place. It does NOT mutate the
 *   server's global tree; it only consumes the serverStatus payload.
 *
 * Usage:
 *   import {
 *       MetricTreeSnapshot,
 *       getServerStatusMetric,
 *       assertMetricPath,
 *       diffMetricSnapshots,
 *   } from "jstests/libs/server_status_metric_tree.js";
 *
 *   const before = MetricTreeSnapshot.capture(conn);
 *   // ... drive workload ...
 *   const after = MetricTreeSnapshot.capture(conn);
 *   const delta = diffMetricSnapshots(before, after, ["network.numRequests"]);
 *   assert.gt(delta["network.numRequests"], 0);
 */

/**
 * Reads a dot-notation path out of an arbitrary BSON-shaped object. Returns
 * `undefined` if any path component is missing -- callers decide whether that
 * means "metric absent" (fine for feature-flagged paths) or "test bug".
 */
export function getPath(doc, path) {
    if (doc === null || doc === undefined) return undefined;
    let cur = doc;
    for (const part of path.split(".")) {
        if (cur === null || typeof cur !== "object" || !cur.hasOwnProperty(part)) {
            return undefined;
        }
        cur = cur[part];
    }
    return cur;
}

/**
 * Reads a single metric out of a fresh serverStatus call. Thin wrapper so callers
 * don't repeat the assert.commandWorked + getPath dance.
 *
 *   const ops = getServerStatusMetric(conn, "network.numRequests");
 *
 * Passing a leading "." anchors the path at the serverStatus root; otherwise the
 * path is implicitly resolved under "metrics." -- matching the C++ MetricTree
 * convention documented in server_status_metric.h.
 */
export function getServerStatusMetric(conn, path) {
    const status = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    const resolved = path.startsWith(".") ? path.substring(1) : `metrics.${path}`;
    return getPath(status, resolved);
}

/**
 * Asserts that `path` resolves to a non-undefined leaf on the latest serverStatus.
 * Optional `predicate(value)` lets callers add a shape check in the same call.
 *
 *   assertMetricPath(conn, ".network.numRequests", (v) => v >= 0);
 */
export function assertMetricPath(conn, path, predicate) {
    const value = getServerStatusMetric(conn, path);
    assert.neq(
        value,
        undefined,
        `serverStatus metric "${path}" missing; full path resolution returned undefined`,
    );
    if (predicate) {
        assert(
            predicate(value),
            `serverStatus metric "${path}" predicate failed; value=${tojson(value)}`,
        );
    }
    return value;
}

/**
 * Captured serverStatus payload + helpers for reading paths off it. The whole
 * payload is retained so a single capture can answer many path queries without
 * re-roundtripping to the server.
 */
export class MetricTreeSnapshot {
    constructor(payload) {
        this._payload = payload;
    }

    static capture(conn) {
        return new MetricTreeSnapshot(assert.commandWorked(conn.adminCommand({serverStatus: 1})));
    }

    /** Returns the raw serverStatus document, for tests that need to escape the helper. */
    raw() {
        return this._payload;
    }

    /**
     * Returns the value at the given path. Leading "." anchors at the root;
     * otherwise resolved under "metrics." to mirror C++ MetricTree semantics.
     */
    get(path) {
        const resolved = path.startsWith(".") ? path.substring(1) : `metrics.${path}`;
        return getPath(this._payload, resolved);
    }

    /** True iff `path` resolves to a non-undefined value. */
    has(path) {
        return this.get(path) !== undefined;
    }

    /**
     * Asserts every path in `paths` is present on this snapshot. Returns the map
     * `{path: value}` for chained assertions.
     */
    assertPathsPresent(paths) {
        const out = {};
        for (const p of paths) {
            const v = this.get(p);
            assert.neq(v, undefined, `metric path "${p}" missing from serverStatus`);
            out[p] = v;
        }
        return out;
    }
}

/**
 * Returns a `{path: after - before}` map over `paths`. Useful for asserting that
 * a workload moved specific counters by an expected amount. Both endpoints must
 * have numeric values at the path or the diff entry is `undefined`. Mirrors the
 * shape of `diffHistogram` / `diffTop` in jstests/libs/stats.js.
 */
export function diffMetricSnapshots(before, after, paths) {
    const out = {};
    for (const p of paths) {
        const b = before.get(p);
        const a = after.get(p);
        if (typeof b !== "number" || typeof a !== "number") {
            out[p] = undefined;
            continue;
        }
        out[p] = a - b;
    }
    return out;
}

/**
 * Asserts that the delta at each path is at least `expectedMin[path]`. Counter
 * metrics in serverStatus are monotonically non-decreasing; tests typically
 * want "at least N more" rather than "exactly N more" because background
 * traffic (TTL monitor, periodic noop writer, etc.) can perturb totals.
 */
export function assertMetricDeltaAtLeast(before, after, expectedMin) {
    const paths = Object.keys(expectedMin);
    const delta = diffMetricSnapshots(before, after, paths);
    for (const p of paths) {
        assert.neq(delta[p], undefined, `metric "${p}" not a numeric leaf in both snapshots`);
        assert.gte(
            delta[p],
            expectedMin[p],
            `metric "${p}" delta=${delta[p]} below expected min=${expectedMin[p]}`,
        );
    }
    return delta;
}
