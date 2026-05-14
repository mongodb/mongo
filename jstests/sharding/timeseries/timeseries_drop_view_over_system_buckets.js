/**
 * Regression test for SERVER-125060.
 *
 * Reproduces the legacy (viewful) timeseries data-loss scenario where dropping a
 * user-created view whose `viewOn` targets a `system.buckets.<name>` collection
 * accidentally drops the underlying buckets collection on the shard. The shard
 * catalog still has the buckets namespace registered, so the resulting state
 * surfaces as a `MissingLocalCollection` inconsistency from
 * `checkMetadataConsistency()` and the time-series collection becomes
 * unreadable.
 *
 * On unpatched v8.0 binaries this test fails: after dropping the view,
 *   - `system.buckets.<ts>` is missing from the shard, and
 *   - `db.checkMetadataConsistency()` reports a `MissingLocalCollection` for it.
 *
 * The patched code path must:
 *   (a) leave `system.buckets.<ts>` intact on the shard after dropping the
 *       view, and
 *   (b) keep `checkMetadataConsistency()` clean, and
 *   (c) keep the time-series collection readable.
 *
 * The bug is impossible on viewless time-series, so we skip when those are
 * enabled.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_sharding,
 *   requires_timeseries,
 *   # The test exercises view DDL whose semantics differ when timeseries are
 *   # viewless. The repro only applies to viewful timeseries.
 *   assumes_no_implicit_collection_creation_after_drop,
 * ]
 */

import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
    skipTestIfViewlessTimeseriesEnabled,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const tsCollName = "realts";
const viewName = "fakets";
const bucketsCollName = getTimeseriesBucketsColl(tsCollName);
const timeField = "t";
const metaField = "m";

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;
const mainDB = mongos.getDB(dbName);

try {
    // The bug is only reproducible on viewful (legacy) timeseries. On viewless
    // timeseries the user-visible namespace is the buckets collection itself
    // and the view DDL path involved here does not exist.
    skipTestIfViewlessTimeseriesEnabled(mainDB, () => st.stop());

    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

    // Shard a time-series collection. ShardCollection auto-creates the
    // backing `system.buckets.realts` collection on the primary shard and
    // registers it in the sharding catalog.
    assert.commandWorked(
        mongos.adminCommand({
            shardCollection: `${dbName}.${tsCollName}`,
            timeseries: {timeField: timeField, metaField: metaField},
            key: {[metaField]: 1},
        }),
    );

    // Insert a sentinel document so we can later prove the time-series
    // collection is still readable end-to-end.
    assert.commandWorked(
        mainDB.getCollection(tsCollName).insert({
            [timeField]: ISODate("2026-01-01T00:00:00Z"),
            [metaField]: "host-A",
            value: 42,
        }),
    );

    // Baseline metadata-consistency check: no inconsistencies after a clean
    // shardCollection + insert.
    let inconsistencies = mainDB.checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));

    // Create a user view whose `viewOn` points directly at the
    // `system.buckets.realts` namespace. This is the trigger configuration
    // from the ticket repro.
    assert.commandWorked(mainDB.createView(viewName, bucketsCollName, []));

    // Sanity: the view, the time-series collection, and the buckets
    // collection all exist before the drop.
    const collsBefore = new Set(mainDB.getCollectionNames());
    assert(collsBefore.has(viewName), () => tojson([...collsBefore]));
    assert(collsBefore.has(tsCollName), () => tojson([...collsBefore]));
    assert(collsBefore.has(bucketsCollName), () => tojson([...collsBefore]));

    // ---- The action under test ----
    // Drop the user-created view. The drop must only remove view metadata.
    // On unpatched v8.0 the code path interprets this as dropping a
    // timeseries view and follows `coll->viewOn()` to drop
    // `system.buckets.realts` from the local shard catalog. The sharding
    // catalog still references the buckets namespace, so a
    // `MissingLocalCollection` inconsistency appears.
    assert.commandWorked(mainDB.runCommand({drop: viewName}));

    // ---- Invariants the patched code must preserve ----

    // (1) The view itself is gone from the mongos view of the database.
    const collsAfter = new Set(mainDB.getCollectionNames());
    assert(!collsAfter.has(viewName), () => tojson([...collsAfter]));

    // (2) The time-series user namespace and its backing buckets collection
    //     are both still present.
    assert(
        collsAfter.has(tsCollName),
        `time-series collection ${tsCollName} was dropped as a side effect of dropping view ${viewName}: ` +
            tojson([...collsAfter]),
    );
    assert(
        collsAfter.has(bucketsCollName),
        `backing buckets collection ${bucketsCollName} was dropped as a side effect of dropping view ${viewName}: ` +
            tojson([...collsAfter]),
    );

    // (3) The buckets collection is still physically present on every shard
    //     that owns chunks for it. We check the primary shard explicitly to
    //     match the failure mode in the ticket repro.
    for (const shard of [st.shard0, st.shard1]) {
        const shardCollections = new Set(shard.getDB(dbName).getCollectionNames());
        // Only assert presence on shards that may own data; shards with no
        // chunks for the collection legitimately do not have a local copy.
        if (shardCollections.has(bucketsCollName) || shard === st.getPrimaryShard(dbName)) {
            assert(
                shardCollections.has(bucketsCollName),
                `${shard.shardName} is missing ${bucketsCollName} after dropping view ${viewName}: ` +
                    tojson([...shardCollections]),
            );
        }
    }

    // (4) checkMetadataConsistency must remain clean. This is the primary
    //     assertion the ticket asks for; on unpatched code it returns one
    //     `MissingLocalCollection` entry for `test.system.buckets.realts`.
    inconsistencies = mainDB.checkMetadataConsistency().toArray();
    assert.eq(
        0,
        inconsistencies.length,
        `dropping the view over ${bucketsCollName} introduced a metadata inconsistency: ` + tojson(inconsistencies),
    );

    // (5) The time-series collection remains readable end-to-end.
    const docs = mainDB.getCollection(tsCollName).find({}).toArray();
    assert.eq(1, docs.length, tojson(docs));
    assert.eq(42, docs[0].value, tojson(docs));
} finally {
    st.stop();
}
