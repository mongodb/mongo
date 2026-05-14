/**
 * Regression test for SERVER-119522.
 *
 * RoutingContext on mongos must correctly classify a namespace as a time-series collection even
 * when its local cache for the underlying `system.buckets.<name>` namespace is stale. The bug
 * surfaced when one mongos created/sharded a time-series collection while a second mongos still
 * held a cached view of the namespace as non-existent (or unsharded), and a subsequent operation
 * issued through the stale mongos was misrouted (or surfaced an internal transaction error such as
 * 6638800 instead of a clean StaleConfig refresh).
 *
 * This test reproduces the scenario by:
 *   1) Priming the stale mongos's routing cache with a namespace that does not exist as
 *      time-series.
 *   2) Performing a dropDatabase + createCollection(timeseries) + shardCollection sequence through
 *      a *different* mongos.
 *   3) Forcing additional cache turnover via addShard/removeShard so that the buckets-namespace
 *      cache entry on the stale mongos is plausibly invalidated yet not refreshed.
 *   4) Issuing an aggregation against the time-series collection through the stale mongos and
 *      asserting that it is routed correctly to the buckets collection rather than failing with a
 *      view-not-found / 6638800 / NamespaceNotFound style misclassification.
 *
 * @tags: [
 *   requires_fcv_51,
 *   requires_sharding,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "testDB_SERVER_119522";
const collName = "tsColl";
const timeField = "time";
const metaField = "hostid";

const st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 1}});

const mongos0 = st.s0;
const mongos1 = st.s1;
const mongos0DB = mongos0.getDB(dbName);
const mongos1DB = mongos1.getDB(dbName);

assert.commandWorked(mongos0.adminCommand({enableSharding: dbName}));

// 1) Prime the routing cache on mongos1 while the time-series collection does not yet exist. A
//    listCollections is enough to populate the negative cache entry for both `dbName.tsColl` and
//    `dbName.system.buckets.tsColl`.
assert.commandWorked(mongos1DB.runCommand({listCollections: 1}));

// 2) Create + shard the time-series collection through mongos0. The buckets namespace
//    `dbName.system.buckets.tsColl` now exists and is sharded, but mongos1's local cache for that
//    namespace is still stale.
assert.commandWorked(
    mongos0DB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}),
);
assert.commandWorked(
    mongos0.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: {[metaField]: 1},
    }),
);

// Seed some data through mongos0 so the buckets collection has content to route to.
const seedDocs = [];
for (let i = 0; i < 10; i++) {
    seedDocs.push({[metaField]: `host_${i % 3}`, [timeField]: ISODate(), value: i});
}
assert.commandWorked(mongos0DB.getCollection(collName).insert(seedDocs));

// 3) Force the routing cache on the stale mongos to invalidate without triggering a refresh of the
//    buckets namespace in particular: an addShard/removeShard cycle bumps the topology version and
//    flushes some cache entries, mimicking the
//    config-transitions-and-add-remove-shards suite that exposed the original bug.
function bounceShard(st) {
    // Spin up a transient shard, then immediately schedule its removal. We don't wait for full
    // drain — the goal is only to perturb the routing cache, not to migrate data.
    const transientName = "transientRS_119522";
    const transientRS = new ReplSetTest({name: transientName, nodes: 1});
    transientRS.startSet({shardsvr: ""});
    transientRS.initiate();

    const addRes = st.s0.adminCommand({addShard: transientRS.getURL(), name: transientName});
    if (addRes.ok) {
        // Best-effort removeShard; the test only needs the cache bump, not a successful drain.
        st.s0.adminCommand({removeShard: transientName});
        st.s0.adminCommand({removeShard: transientName});
    }

    transientRS.stopSet();
}

try {
    bounceShard(st);
} catch (e) {
    // The shard bounce is a best-effort cache-perturbation step. If the topology refuses the
    // transient shard (e.g., port conflict in CI), the test should still exercise the stale-cache
    // path via the listCollections priming above.
    jsTestLog(`SERVER-119522 test: bounceShard skipped due to ${e}`);
}

// 4) Issue an aggregation against the time-series collection through the stale mongos. Prior to
//    the fix this would either return a stale view-not-found, throw the internal 6638800 from
//    runTimeseriesRetryableUpdates, or be silently routed as UNSHARDED to the dbPrimary with the
//    wrong shardVersion. With the fix the RoutingContext on mongos1 refreshes the buckets-namespace
//    cache as part of init and the aggregate is routed to the buckets collection.
const aggRes = assert.commandWorked(
    mongos1DB.runCommand({
        aggregate: collName,
        pipeline: [{$match: {}}, {$count: "n"}],
        cursor: {},
    }),
    "Aggregation against time-series collection failed on the stale mongos — RoutingContext did not refresh the system.buckets cache.",
);

const firstBatch = aggRes.cursor.firstBatch;
assert.eq(firstBatch.length, 1, () => `Expected one row from $count, got ${tojson(firstBatch)}`);
assert.eq(
    firstBatch[0].n,
    seedDocs.length,
    () => `Expected ${seedDocs.length} measurements via the stale mongos, got ${tojson(firstBatch)}`,
);

// And confirm that the same aggregation is also clean when re-issued: the refresh should be
// sticky, not one-shot.
const aggRes2 = assert.commandWorked(
    mongos1DB.runCommand({
        aggregate: collName,
        pipeline: [{$match: {}}, {$count: "n"}],
        cursor: {},
    }),
);
assert.eq(aggRes2.cursor.firstBatch[0].n, seedDocs.length);

st.stop();
