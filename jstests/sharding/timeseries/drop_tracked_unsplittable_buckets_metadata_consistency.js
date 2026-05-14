/**
 * Reproduces SERVER-95267: Metadata inconsistency when dropping a tracked unsplittable
 * timeseries-buckets collection.
 *
 * Scenario: a timeseries collection's system.buckets.* namespace is registered in
 * config.collections as `unsplittable: true` (e.g. after moveCollection on the
 * buckets namespace, or after creation on a non-primary shard). The logical view is
 * either absent, shadowed by an unrelated view, or shadowed by a normal collection.
 *
 * Bug: issuing drop against either the logical namespace OR the system.buckets
 * namespace executes a local-only drop on the primary shard, leaving a dangling
 * entry in config.collections referencing the now-gone buckets collection.
 *
 * Detection: checkMetadataConsistency should report a MisplacedCollection /
 * HiddenShardedCollection inconsistency (no chunks for a tracked collection that
 * no shard hosts). The repro fails today; once the fix lands, it should pass
 * without inconsistencies.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_80,
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2});
const mongos = st.s0;
const dbName = jsTestName();
const adminDB = mongos.getDB("admin");
const testDB = mongos.getDB(dbName);

assert.commandWorked(adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

const kTs = {timeField: "t", metaField: "m"};

function assertNoInconsistencies(label) {
    const res = adminDB.checkMetadataConsistency({checkIndexes: 1}).toArray();
    assert.eq(0, res.length, `[${label}] unexpected inconsistencies: ${tojson(res)}`);
}

function configCollectionsEntries(nss) {
    return mongos.getDB("config").collections.find({_id: nss}).toArray();
}

function ensureBucketsTrackedAsUnsplittable(coll) {
    const fullNss = `${dbName}.system.buckets.${coll}`;
    const entries = configCollectionsEntries(fullNss);
    assert.eq(1, entries.length, `expected one config.collections row for ${fullNss}: ${tojson(entries)}`);
    assert.eq(true, entries[0].unsplittable, `expected unsplittable=true for ${fullNss}: ${tojson(entries[0])}`);
}

function dropLeavesNoDanglingRow(coll, dropTarget) {
    const bucketsNss = `${dbName}.system.buckets.${coll}`;
    assert.commandWorked(testDB.runCommand({drop: dropTarget}));
    const remaining = configCollectionsEntries(bucketsNss);
    assert.eq(0, remaining.length, `dangling config.collections row for ${bucketsNss} after dropping ${dropTarget}: ${tojson(remaining)}`);
    assertNoInconsistencies(`post-drop[${coll} via ${dropTarget}]`);
}

// Scenario A — buckets-only tracked, nothing in the main namespace.
(function scenarioA_dropViaBucketsNs() {
    const coll = "tsA";
    assert.commandWorked(testDB.createCollection(coll, {timeseries: kTs}));
    assert.commandWorked(adminDB.runCommand({moveCollection: `${dbName}.${coll}`, toShard: st.shard1.shardName}));
    ensureBucketsTrackedAsUnsplittable(coll);
    // Force the "nothing in main namespace" condition by dropping the view first
    // via direct buckets-collection drop on the buckets namespace.
    dropLeavesNoDanglingRow(coll, `system.buckets.${coll}`);
})();

// Scenario B — buckets tracked, unrelated view sits in the main namespace.
(function scenarioB_dropViaMainNs_unrelatedView() {
    const coll = "tsB";
    assert.commandWorked(testDB.createCollection(coll, {timeseries: kTs}));
    assert.commandWorked(adminDB.runCommand({moveCollection: `${dbName}.${coll}`, toShard: st.shard1.shardName}));
    // Drop only the logical view, leaving the tracked unsplittable buckets behind,
    // then install an unrelated view at the same main-namespace name.
    assert.commandWorked(testDB.runCommand({drop: coll}));
    // Re-create the buckets-only state — moveCollection again to ensure it is tracked.
    assert.commandWorked(testDB.createCollection(`system.buckets.${coll}`, {clusteredIndex: true}));
    assert.commandWorked(adminDB.runCommand({moveCollection: `${dbName}.system.buckets.${coll}`, toShard: st.shard1.shardName}));
    assert.commandWorked(testDB.createView(coll, "someOtherSource", []));
    ensureBucketsTrackedAsUnsplittable(coll);
    dropLeavesNoDanglingRow(coll, coll);
})();

// Scenario C — buckets tracked, normal (non-timeseries) collection at the main namespace.
(function scenarioC_dropViaMainNs_normalColl() {
    const coll = "tsC";
    assert.commandWorked(testDB.createCollection(`system.buckets.${coll}`, {clusteredIndex: true}));
    assert.commandWorked(adminDB.runCommand({moveCollection: `${dbName}.system.buckets.${coll}`, toShard: st.shard1.shardName}));
    assert.commandWorked(testDB.createCollection(coll));
    ensureBucketsTrackedAsUnsplittable(coll);
    dropLeavesNoDanglingRow(coll, coll);
})();

st.stop();
