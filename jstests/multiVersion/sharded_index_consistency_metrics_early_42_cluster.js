/**
 * Tests that the index consistency serverStatus metrics don't consider indexes on shards with
 * binary versions < 4.2.4 as inconsistent. This is done because the aggregation pipeline used by
 * the config server to detect when a sharded collection has inconsistent indexes relies on an
 * extension to $indexStats output that was added by SERVER-44915 and backported to v4.2.4, so any
 * shards using an earlier binary will not include the expected output and their responses should be
 * ignored instead of incorrectly interpreted to mean the collection has inconsistent indexes.
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/verify_versions.js");
load("jstests/noPassthrough/libs/sharded_index_consistency_metrics_helpers.js");

const preIndexStatsExtensionBackport42Version = "4.2.3";
const latestVersion = "latest";

// Set up a mixed version cluster with one shard in 4.2.3 and the rest of the cluster in the latest
// 4.2 version.
const intervalMS = 1000;
const st = new ShardingTest({
    shards: [
        {binVersion: preIndexStatsExtensionBackport42Version},
        {binVersion: "latest"},
        {binVersion: "latest"},
    ],
    other: {
        mongosOptions: {binVersion: "latest"},
        configOptions: {
            binVersion: "latest",
            setParameter: {"shardedIndexConsistencyCheckIntervalMS": intervalMS}
        },
    }
});
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

assert.binVersion(st.shard0, preIndexStatsExtensionBackport42Version);
assert.binVersion(st.shard1, "latest");
assert.binVersion(st.shard2, "latest");
assert.binVersion(st.s, "latest");

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: "hashed"}}));

// Defaults to 0.
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 0);

// Create an inconsistent index for ns. This won't be reflected by the serverStatus counter because
// the index is on a 4.2.3 shard, which should be ignored by the periodic index checker.
assert.commandWorked(st.shard0.getCollection(ns).createIndex({x: 1}));
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 0);

// Put an inconsistent index on a shard at the latest binary version. This should not be ignored
// because there is at least one other shard with binary version > 4.2.4 that does not have this
// index.
assert.commandWorked(st.shard1.getCollection(ns).createIndex({x: 1}));
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 1);

// Resolve the inconsistency.
assert.commandWorked(st.shard2.getCollection(ns).createIndex({x: 1}));
checkServerStatusNumCollsWithInconsistentIndexes(st.configRS.getPrimary(), 0);

st.stop();
})();
