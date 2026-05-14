/**
 * Harness for tests that inject previously-allowed but currently-disallowed catalog metadata
 * and verify the server continues to behave correctly.
 *
 * This file is intentionally a *library* (no top-level test body) - example tests live in
 * stale_catalog_injection_*.js next to it. The harness mediates two concerns:
 *
 *   1. Producing on-disk catalog rows that mirror what older versions of the server allowed
 *      but which validation rejects today (SERVER-54712 weights on non-text indexes,
 *      SERVER-68477 NaN expireAfterSeconds, SERVER-77828 float expireAfterSeconds,
 *      SERVER-85837 obsolete feature_compatibility documents, SERVER-11064 pre-3.4 index
 *      orderings of "" or 0).
 *
 *   2. Asserting server liveness afterwards - validate / collMod / restart should all stay
 *      healthy, and known-good operations against the same collection should still succeed.
 *
 * Tests opt into the harness by calling runStaleCatalogInjectionScenario({...}) with a
 * `setup` (mutate the catalog) and `verify` (assert behaviour) hook. The harness owns the
 * ShardingTest / ReplSetTest lifecycle and the failpoint book-keeping.
 *
 * Follow-up to SERVER-94487, dependent on SERVER-99064. See SERVER-95283.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Failpoints that lift validation so we can persist catalog metadata in the same shape older
 * server versions used to write. Each entry maps a friendly name onto the actual failpoint
 * defined in src/mongo/db/index_key_validate.cpp (and adjacent files).
 */
export const StaleCatalogFailPoints = Object.freeze({
    // Skips field-name validation on createIndex - allows unknown options like
    // `weights` on a non-text index (SERVER-54712) and unknown fields in general.
    skipIndexFieldNames: "skipIndexCreateFieldNameValidation",

    // Skips TTL expireAfterSeconds validation - allows NaN (SERVER-68477) and float
    // (SERVER-77828) values to be persisted in the catalog.
    skipTtlExpireValidation: "skipTTLIndexExpireAfterSecondsValidation",
});

/**
 * Default scenario shape. Tests override the `setup` and `verify` callbacks; everything
 * else has a working default so a typical test is ~30 lines.
 */
const kDefaultOptions = {
    // "replset" or "sharded"
    topology: "replset",

    // Topology sizing.
    nodes: 2,
    shards: 1,

    // Failpoints to install on every data-bearing node before `setup` runs. Use the
    // values from StaleCatalogFailPoints (e.g. [StaleCatalogFailPoints.skipTtlExpireValidation]).
    failPoints: [],

    // Whether to restart all data-bearing nodes after `setup` finishes. Many of the
    // historical bugs only surface on a clean reload of the catalog from disk, so this
    // defaults to true.
    restartAfterSetup: true,

    // (rst|st, primaryConn) => void. Runs while failpoints are active. Should write whatever
    // legacy-shaped metadata the test wants to inject.
    setup: () => {
        throw new Error("stale-catalog injection scenario must define a `setup` callback");
    },

    // (rst|st, primaryConn) => void. Runs after failpoints are dropped (and after restart,
    // if requested). Should make the actual liveness / repair / warning assertions.
    verify: () => {
        throw new Error("stale-catalog injection scenario must define a `verify` callback");
    },
});

/**
 * Returns the list of data-bearing connections we need to install failpoints on. For a
 * replica set that's primary + secondaries; for a sharded cluster it's the primary of every
 * shard. Config-server-side catalog injection is intentionally out-of-scope - those bugs
 * are tracked separately.
 */
function dataBearingNodes(harness) {
    if (harness.kind === "replset") {
        return harness.rst.nodes;
    }
    const nodes = [];
    for (const rs of harness.st._rs) {
        for (const node of rs.test.nodes) {
            nodes.push(node);
        }
    }
    return nodes;
}

function startTopology(opts) {
    if (opts.topology === "replset") {
        const rst = new ReplSetTest({nodes: opts.nodes});
        rst.startSet();
        rst.initiate();
        return {kind: "replset", rst, primary: () => rst.getPrimary()};
    }
    if (opts.topology === "sharded") {
        const st = new ShardingTest({
            shards: opts.shards,
            rs: {nodes: opts.nodes},
            initiateWithDefaultElectionTimeout: true,
        });
        return {kind: "sharded", st, primary: () => st.rs0.getPrimary()};
    }
    throw new Error("unknown topology: " + opts.topology);
}

function stopTopology(harness) {
    if (harness.kind === "replset") {
        harness.rst.stopSet();
    } else {
        harness.st.stop();
    }
}

function installFailPoints(harness, names) {
    const handles = [];
    for (const node of dataBearingNodes(harness)) {
        for (const name of names) {
            handles.push(configureFailPoint(node.getDB("admin"), name));
        }
    }
    return handles;
}

function dropFailPoints(handles) {
    for (const fp of handles) {
        try {
            fp.off();
        } catch (e) {
            // Best-effort - the test may have already restarted the node the failpoint was on.
        }
    }
}

function restartAll(harness) {
    if (harness.kind === "replset") {
        const secondaries = harness.rst.getSecondaries();
        for (const sec of secondaries) {
            harness.rst.restart(sec);
        }
        harness.rst.awaitSecondaryNodes();
        // Step the primary down + back up so its on-disk catalog is also re-read.
        harness.rst.stepUp(harness.rst.getSecondary());
        harness.rst.awaitNodesAgreeOnPrimary();
        return;
    }
    for (const rs of harness.st._rs) {
        const secondaries = rs.test.getSecondaries();
        for (const sec of secondaries) {
            rs.test.restart(sec);
        }
        rs.test.awaitSecondaryNodes();
    }
}

/**
 * Run a single stale-catalog injection scenario.
 *
 * @param {object} userOpts - see kDefaultOptions for the schema.
 */
export function runStaleCatalogInjectionScenario(userOpts) {
    const opts = Object.assign({}, kDefaultOptions, userOpts);
    const harness = startTopology(opts);

    const failPointHandles = installFailPoints(harness, opts.failPoints);
    try {
        opts.setup(harness, harness.primary());
    } finally {
        dropFailPoints(failPointHandles);
    }

    // Persist on-disk before any restart so the secondaries see the legacy metadata.
    if (harness.kind === "replset") {
        harness.rst.awaitReplication();
        assert.commandWorked(harness.primary().adminCommand({fsync: 1}));
    } else {
        for (const rs of harness.st._rs) {
            rs.test.awaitReplication();
            assert.commandWorked(rs.test.getPrimary().adminCommand({fsync: 1}));
        }
    }

    if (opts.restartAfterSetup) {
        restartAll(harness);
    }

    try {
        opts.verify(harness, harness.primary());
    } finally {
        stopTopology(harness);
    }
}

/**
 * Convenience: create an index with otherwise-disallowed options. The relevant failpoints
 * must already be installed by the caller (the harness does this automatically when the
 * caller declares the failpoints in `opts.failPoints`).
 */
export function createIndexWithLegacyOptions(coll, keyPattern, options) {
    return coll.createIndex(keyPattern, options);
}
