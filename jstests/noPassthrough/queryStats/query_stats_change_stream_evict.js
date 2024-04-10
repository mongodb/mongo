// Tests the collection of query stats for a change stream query if the entry is evicted while the
// cursor is still active.
// @tags: [
//   uses_change_streams,
//   requires_replication,
//   requires_sharding,
//   requires_fcv_72
// ]
import {
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    getLatestQueryStatsEntry,
    getQueryStats,
    resetQueryStatsStore
} from "jstests/libs/query_stats_utils.js";

function runTest(conn) {
    const db = conn.getDB("test");
    assertDropAndRecreateCollection(db, "coll");

    // Check creation of change stream cursor is recorded.
    let cursor = db.coll.watch([]);

    // Check that change stream entry was recorded.
    let queryStatsEntry = getLatestQueryStatsEntry(db);

    assert.eq("coll", queryStatsEntry.key.queryShape.cmdNs.coll);
    let stringifiedPipeline = JSON.stringify(queryStatsEntry.key.queryShape.pipeline, null, 0);
    assert(stringifiedPipeline.includes("_internalChangeStream"));
    // TODO SERVER-76263 Support reporting 'collectionType' on a sharded cluster.
    if (!FixtureHelpers.isMongos(db)) {
        assert.eq("changeStream", queryStatsEntry.key.collectionType);
    }

    // Reset the store to evict the change streams metric.
    resetQueryStatsStore(db, "1MB");

    // Insert document into a collection which should update the cursor.
    assert.commandWorked(db.coll.insert({_id: 0, a: 1}));
    assert.soon(() => cursor.hasNext());

    // There should be nothing stored in the query stats store.
    let queryStats = getQueryStats(db);
    assert.eq(queryStats, []);

    // Close cursor.
    cursor.close();
}

{
    // Test the non-sharded case.
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
    rst.initiate();
    rst.getPrimary().getDB("admin").setLogLevel(3, "queryStats");
    runTest(rst.getPrimary());
    rst.stopSet();
}

{
    // Test on a sharded cluster.
    const st = new ShardingTest({
        mongos: 1,
        shards: 2,
        config: 1,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        mongosOptions: {
            setParameter: {
                internalQueryStatsRateLimit: -1,
            }
        },
    });
    runTest(st.s);
    st.stop();
}