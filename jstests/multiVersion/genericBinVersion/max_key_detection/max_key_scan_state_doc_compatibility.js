/**
 * Defensive narrow test for the lifecycle of config.maxKeyOrphanScanState and
 * config.maxKeyZoneScanState across a shard binary downgrade/upgrade and explicit state-doc
 * deletion.
 *
 * This test deliberately keeps the binary downgrade scoped to the shard replica set to avoid
 * having to also downgrade mongos and configsvr.
 *
 * Covers both the shard-server flavour (MaxKey orphan detector scan,
 * config.maxKeyOrphanScanState) and the config-server flavour (Balancer scan,
 * config.maxKeyZoneScanState) on the same sharded cluster:
 *   1. Boot a latest-binary sharded cluster with featureFlagMaxKeyDetection enabled. Stepup the
 *      shard and configsvr. Verify both state docs are persisted with scanCompletedAt.
 *   2. Downgrade the shard's binary to last-lts via ReplSetTest.upgradeSet (which strips
 *      featureFlagMaxKeyDetection automatically). Verify the older shard binary boots cleanly with
 *      config.maxKeyOrphanScanState present, the state doc is unchanged, and ordinary writes still
 *      work.
 *   3. Re-upgrade the shard binary to latest. Verify the existing scanCompletedAt short-circuits
 *      the scan on the next stepup (no rewrite of either doc) — the one-shot guard survives a
 *      binary downgrade/upgrade cycle.
 *   4. Drop both state docs and verify the next stepup writes fresh ones.
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {ShardingTest} from "jstests/libs/shardingtest.js";

const scanStateId = "scanState";

function readStateDoc(rs, collName) {
    return rs.getPrimary().getDB("config").getCollection(collName).findOne({_id: scanStateId});
}

function stepDownAndUp(rs) {
    const newPrimary = rs.getSecondary();
    rs.stepUp(newPrimary);
    rs.waitForPrimary();
}

// Freeze every non-primary node so it can never call an election. Freezing only blocks election
// eligibility, not replication, so majority writes on the primary still succeed. Computed against
// the current primary (rather than getSecondaries()) so a node transiently out of SECONDARY state
// is still frozen and can't later flip the primary.
function freezeSecondaries(rs) {
    const primary = rs.getPrimary();
    rs.nodes.filter((node) => node.host !== primary.host).forEach((node) => rs.freeze(node));
}

// The checkOrphansAreDeleted teardown hook does a no-retry primary read over the process-global
// ReplicaSetMonitor, which this test's many stepUps can leave pointing at a demoted node, failing
// the read. Force that shared monitor to re-converge on the current primary now (a failed read here
// triggers an immediate refresh); pair with freezeSecondaries() so the primary can't move after.
// keyFile mirrors the hook: under multiversion_auth the connection must auth as __system first.
function warmReplicaSetMonitor(rs, keyFile) {
    const conn = new Mongo(rs.getURL());
    const readUntilPrimaryReached = () =>
        assert.soon(() => {
            try {
                conn.getDB("config").migrationCoordinators.find().toArray();
                return true;
            } catch (e) {
                return false;
            }
        }, `ReplicaSetMonitor for ${rs.getURL()} never converged on a reachable primary`);
    if (keyFile) {
        authutil.asCluster(conn, keyFile, readUntilPrimaryReached);
    } else {
        readUntilPrimaryReached();
    }
}

function clearStateDoc(rs, collName) {
    assert.commandWorked(
        rs
            .getPrimary()
            .getDB("config")
            .runCommand({
                delete: collName,
                deletes: [{q: {_id: scanStateId}, limit: 0}],
                writeConcern: {w: "majority"},
            }),
    );
    rs.awaitReplication();
}

function awaitStateDoc(rs, collName, contextMsg, timeoutMs = 60 * 1000) {
    assert.soon(
        () => {
            const d = readStateDoc(rs, collName);
            return d !== null && d.scanCompletedAt !== undefined;
        },
        `${contextMsg}: ${collName} doc with scanCompletedAt never appeared`,
        timeoutMs,
    );
    return readStateDoc(rs, collName);
}

const featureFlagParam = {featureFlagMaxKeyDetection: true};

const st = new ShardingTest({
    mongos: 1,
    config: 2,
    shards: 1,
    other: {
        configOptions: {setParameter: featureFlagParam},
        rsOptions: {setParameter: featureFlagParam},
        rs: {nodes: 2},
    },
    initiateWithDefaultElectionTimeout: true,
});
st.configRS.awaitReplication();

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

const dbName = jsTestName();
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.getDB(dbName).getCollection("userColl").insert({a: 1}));

stepDownAndUp(st.rs0);
stepDownAndUp(st.configRS);

const firstOrphanDoc = awaitStateDoc(st.rs0, "maxKeyOrphanScanState", "first stepup, shard0");
const firstZoneDoc = awaitStateDoc(st.configRS, "maxKeyZoneScanState", "first stepup, configsvr");

// FCV must be downgraded before the binary; upgradeSet strips featureFlagMaxKeyDetection for pre-9.0.
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);
st.rs0.upgradeSet({binVersion: "last-lts"});

const orphanOnOld = readStateDoc(st.rs0, "maxKeyOrphanScanState");
assert.neq(null, orphanOnOld, "Older shard binary must still see the orphan state doc by name");
assert.docEq(firstOrphanDoc, orphanOnOld, "Binary downgrade must not rewrite the orphan state doc");

// Write directly to the shard, not via mongos: a 9.0 mongos refuses connections to an 8.0 shard
// (wire-version mismatch; correct downgrade order is mongos → shard → config).
assert.commandWorked(
    st.rs0.getPrimary().getDB("config").getCollection("compat_probe").insert({_id: 1, ok: true}),
);

st.rs0.upgradeSet({binVersion: "latest", setParameter: featureFlagParam});
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

st.rs0.nodes.forEach((node) =>
    assert.commandWorked(
        node.adminCommand({setParameter: 1, logComponentVerbosity: {sharding: {verbosity: 2}}}),
    ),
);

stepDownAndUp(st.rs0);
stepDownAndUp(st.configRS);

// Confirm the short-circuit log fired this term — a broken guard would never emit it.
const orphanGuardTerm = assert.commandWorked(
    st.rs0.getPrimary().adminCommand({replSetGetStatus: 1}),
).term;
checkLog.containsJson(st.rs0.getPrimary(), 12799006, {term: Number(orphanGuardTerm)});
const zoneGuardTerm = assert.commandWorked(
    st.configRS.getPrimary().adminCommand({replSetGetStatus: 1}),
).term;
checkLog.containsJson(st.configRS.getPrimary(), 12829503, {term: Number(zoneGuardTerm)});

assert.docEq(
    firstOrphanDoc,
    readStateDoc(st.rs0, "maxKeyOrphanScanState"),
    "Existing orphan scanCompletedAt must short-circuit the scan on the next stepup",
);
assert.docEq(
    firstZoneDoc,
    readStateDoc(st.configRS, "maxKeyZoneScanState"),
    "Existing zone scanCompletedAt must short-circuit the scan on the next stepup",
);

clearStateDoc(st.rs0, "maxKeyOrphanScanState");
clearStateDoc(st.configRS, "maxKeyZoneScanState");
assert.eq(null, readStateDoc(st.rs0, "maxKeyOrphanScanState"));
assert.eq(null, readStateDoc(st.configRS, "maxKeyZoneScanState"));

stepDownAndUp(st.rs0);
stepDownAndUp(st.configRS);

// initiateWithDefaultElectionTimeout keeps the 10s election timeout, so an idle secondary
// could otherwise spontaneously call an election near teardown and flip the primary out from under
// the teardown checks. Lock each set on its current (already-settled) primary right after the last
// stepUp.
freezeSecondaries(st.rs0);
freezeSecondaries(st.configRS);

const freshOrphan = awaitStateDoc(st.rs0, "maxKeyOrphanScanState", "after drop, shard0");
const freshZone = awaitStateDoc(st.configRS, "maxKeyZoneScanState", "after drop, configsvr");
assert.neq(
    firstOrphanDoc.scanCompletedAt,
    freshOrphan.scanCompletedAt,
    "Dropped orphan state doc must be rewritten with a fresh scanCompletedAt",
);
assert.neq(
    firstZoneDoc.scanCompletedAt,
    freshZone.scanCompletedAt,
    "Dropped zone state doc must be rewritten with a fresh scanCompletedAt",
);

// With the primaries now frozen in place, converge the process-global ReplicaSetMonitor that the
// checkOrphansAreDeleted hook will reuse so its no-retry primary read does not race on a stale view.
warmReplicaSetMonitor(st.rs0, st.keyFile);

st.stop();
